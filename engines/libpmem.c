/*
 * libpmem: IO engine that uses PMDK libpmem to read and write data
 *
 * Copyright (C) 2017 Nippon Telegraph and Telephone Corporation.
 * Copyright 2018-2021, Intel Corporation
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
 * [한국어 설명] libpmem I/O 엔진 구현 (libpmem.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 fio의 I/O 엔진 플러그인 하나로, PMDK(Persistent Memory Development Kit)의
 * libpmem 라이브러리를 이용해 영속 메모리(PMEM/NVDIMM) DAX 파일에 대한 읽기/쓰기를
 * mmap+memcpy 모델로 수행한다. 파일을 pmem_map_file()로 한 번 매핑해 둔 뒤, 각 io_u 요청은
 * 매핑 영역 내부의 memcpy/pmem_memcpy로 처리하므로 read(2)/write(2) 시스템 호출이
 * 일절 발생하지 않는다. sync=1이면 pmem_drain()으로 영속성 배리어를 강제하고,
 * direct=1이면 NONTEMPORAL 저장(캐시 우회)으로 PMEM 대역폭을 직접 쓴다. 본 엔진은
 * PMEM 하드웨어의 실질 지연/대역폭을 벤치마크하는 것이 주목적이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 코어 흐름에서 backend.c의 잡 스레드 루프가 load_ioengine("libpmem") →
 * td_io_init() → td_io_queue() → td_io_commit() → td_io_getevents() 순으로 호출한다.
 * 본 엔진은 동기 엔진(FIO_SYNCIO)이라 queue() 내부에서 실제 memcpy까지 완료한 뒤
 * FIO_Q_COMPLETED를 반환하며, 별도 getevents/event 콜백 구현이 필요 없다(일반 sync 경로 사용).
 * 실행 컨텍스트는 각 fio 잡 스레드(thread_data td 1개 = 스레드 1개)이며,
 * DAX-mmap된 사용자 공간에서 동작한다. 커널은 매핑 설정(mmap 시점)에만 개입하고
 * I/O 경로에는 개입하지 않는다(DAX 특징).
 *
 * === 타 모듈과의 연결 ===
 * - fio.h: thread_data, fio_file, io_u, ioengine_ops 등 fio 공용 타입 제공.
 * - verify.h: verify 모드 시 데이터 무결성 체크 헬퍼.
 * - libpmem(PMDK): 실제 PMEM 매핑/복사/드레인 구현체.
 * - 데이터 흐름: fio_file(f) → (pmem_map_file) → fdd->libpmem_ptr → io_u->mmap_data →
 *   io_u->xfer_buf(읽기) / xfer_buf→mmap_data(쓰기) 방향으로 바이트 복사.
 * - 공유 상태: fdd(fio_libpmem_data)는 파일별로 FILE_ENG_DATA(f) 슬롯에 저장되어
 *   같은 fio_file을 공유하는 io_u들이 모두 동일한 매핑을 참조한다. fio 잡 1개가
 *   fio_file 1개를 전담하므로 파일별로는 단일 스레드 접근(락 불필요).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_libpmem_data: 파일별 매핑 상태(시작 주소/크기/오프셋).
 * - fio_libpmem_init():   블록 크기가 페이지 배수인지 + fsync 플래그 충돌 검증.
 * - fio_libpmem_file():   pmem_map_file() 실제 수행, is_pmem 여부 확인.
 * - fio_libpmem_open_file(): fdd 할당 후 위 헬퍼로 파일 매핑.
 * - fio_libpmem_prep():   io_u->offset을 매핑 내부 가상주소(mmap_data)로 변환.
 * - fio_libpmem_queue():  ddir 분기에 따라 memcpy/pmem_memcpy/pmem_drain 수행.
 * - fio_libpmem_close_file(): pmem_unmap + generic_close_file.
 *
 * === fio에서의 사용법 ===
 * --ioengine=libpmem, directory=/mnt/pmem0/ (DAX 마운트), direct=1, sync=1, iodepth=1 권장.
 */

/*
 * libpmem engine
 *
 * IO engine that uses libpmem (part of PMDK collection) to write data
 *	and libc's memcpy to read. It requires PMDK >= 1.5.
 *
 * To use:
 *   ioengine=libpmem
 *
 * Other relevant settings:
 *   iodepth=1
 *   direct=1
 *   sync=1
 *   directory=/mnt/pmem0/
 *   bs=4k
 *
 *   sync=1 means that pmem_drain() is executed for each write operation.
 *   Otherwise is not and should be called on demand.
 *
 *   direct=1 means PMEM_F_MEM_NONTEMPORAL flag is set in pmem_memcpy().
 *
 *   The pmem device must have a DAX-capable filesystem and be mounted
 *   with DAX enabled. Directory must point to a mount point of DAX FS.
 *
 *   Example:
 *     mkfs.xfs /dev/pmem0
 *     mkdir /mnt/pmem0
 *     mount -o dax /dev/pmem0 /mnt/pmem0
 *
 * See examples/libpmem.fio for complete usage example.
 */

#include <stdio.h>      /* [한국어] 디버그 로그/로그 포매팅용 printf 계열 — dprint 매크로가 내부적으로 사용 */
#include <stdlib.h>     /* [한국어] calloc/free 메모리 관리용 — fdd 할당/해제에 필요 */
#include <unistd.h>     /* [한국어] 파일 디스크립터 상수·기본 POSIX API — fio_file.fd 조작 맥락 */
#include <errno.h>      /* [한국어] errno 전역 및 에러 코드(EINVAL, EIO 등) — pmem_* 실패 시 반환값 */
#include <libpmem.h>    /* [한국어] PMDK libpmem API: pmem_map_file, pmem_memcpy, pmem_drain, pmem_unmap */

#include "../fio.h"     /* [한국어] fio 코어 타입(thread_data, io_u, ioengine_ops, fio_file, dprint 등) */
#include "../verify.h"  /* [한국어] verify 모드 헬퍼 — 이 엔진은 verify_header 해석에 공용 경로 사용 */

/*
 * [한국어] libpmem 엔진의 파일별 매핑 상태 구조체.
 * mmap.c의 fio_mmap_data와 유사한 역할이며, FILE_SET_ENG_DATA(f, fdd)로 fio_file에 부착된다.
 * 한 fio_file은 한 잡 스레드에서만 다뤄지므로 별도 락 없이 접근한다.
 */
struct fio_libpmem_data {
	void *libpmem_ptr;
	/* [한국어] pmem_map_file()이 반환한 매핑 영역의 시작 가상주소(사용자 공간).
	 * 설정자: fio_libpmem_file()에서 pmem_map_file 성공 시 저장.
	 * 읽는 자: fio_libpmem_prep()가 io_u 오프셋을 더해 io_u->mmap_data로 만든다;
	 *          fio_libpmem_close_file()가 pmem_unmap 대상 주소로 사용.
	 * 값 범위: 유효한 mmap된 주소 또는 NULL(초기/해제 후).
	 * 동기화: 파일 1개당 잡 스레드 1개 전담이므로 락 불필요. */

	size_t libpmem_sz;
	/* [한국어] 매핑된 영역의 바이트 크기 (보통 f->io_size와 동일).
	 * 설정자: fio_libpmem_open_file()에서 f->io_size로 초기화.
	 * 읽는 자: pmem_unmap(close_file)에서 해제 크기로 사용.
	 * 값 범위: >0 (DAX 파일의 매핑 가능 크기 이하).
	 * 동기화: 단일 스레드 소유. */

	off_t libpmem_off;
	/* [한국어] 파일 내에서 매핑이 시작되는 오프셋(현 구현은 항상 0).
	 * 설정자: open_file에서 0으로 초기화(부분 매핑 미사용).
	 * 읽는 자: prep()가 io_u->offset 보정에 사용.
	 * 값 범위: 0 이상. 현재 코드 경로에서는 0 고정.
	 * 동기화: 단일 스레드 소유. */
};

/*
 * [한국어]
 * fio_libpmem_init - 잡 시작 시 libpmem 엔진 옵션 정합성 검증 콜백.
 *
 * @td: 현재 잡의 thread_data (파싱 완료된 옵션 td->o 포함).
 * @return: 0 성공, 1 실패(해당 잡을 중단시킨다).
 *
 * 이 엔진은 페이지 단위 매핑 기반이라, fsync/fdatasync가 설정된 상태에서
 * 블록 크기(rw_min_bs)가 페이지 크기 배수가 아니면 의미 있는 동작이 불가능하다.
 * 따라서 시작 시점에 걸러낸다.
 *
 * 실행 컨텍스트: 잡 스레드 시작 직후(단일 스레드).
 * 호출 체인: td_io_init() → ioengine_ops.init → [fio_libpmem_init]
 */
static int fio_libpmem_init(struct thread_data *td)
{
	struct thread_options *o = &td->o;  /* [한국어] 옵션 구조체 포인터 단축 */

	/* [한국어] 디버그: 블록 크기/fsync 관련 옵션을 FD_IO 카테고리로 출력 */
	dprint(FD_IO, "o->rw_min_bs %llu\n o->fsync_blocks %u\n o->fdatasync_blocks %u\n",
			o->rw_min_bs, o->fsync_blocks, o->fdatasync_blocks);
	dprint(FD_IO, "DEBUG fio_libpmem_init\n");  /* [한국어] 진입 로그 */

	/* [한국어] rw_min_bs가 페이지 배수가 아니며(AND page_mask != 0) 동시에
	 * fsync/fdatasync 블록 기반 옵션이 켜져 있으면 정렬 위배로 거절 */
	if ((o->rw_min_bs & page_mask) &&
	    (o->fsync_blocks || o->fdatasync_blocks)) {
		log_err("libpmem: mmap options dictate a minimum block size of "
				"%llu bytes\n",	(unsigned long long) page_size);
		return 1;  /* [한국어] 실패 반환 → fio 코어가 잡 실행 포기 */
	}
	return 0;  /* [한국어] 정상 */
}

/*
 * This is the pmem_map_file execution function, a helper to
 * fio_libpmem_open_file function.
 */
/*
 * [한국어]
 * fio_libpmem_file - pmem_map_file() 실제 수행 헬퍼.
 *
 * @td:     잡 컨텍스트 (에러 전파용).
 * @f:      매핑 대상 fio_file. FILE_ENG_DATA(f)에 fdd가 이미 붙어 있어야 함.
 * @length: 매핑 요청 크기 (바이트).
 * @off:    매핑 시작 오프셋 (현 구현 호출부는 항상 0 전달).
 * @return: 0 성공, 그 외 errno 또는 td->error.
 *
 * 기존 매핑이 남아 있으면 먼저 pmem_unmap으로 해제한 뒤 pmem_map_file을 호출한다.
 * PMEM_FILE_CREATE로 파일이 없으면 생성하고, is_pmem==0이면 DAX가 아님을 경고한다.
 *
 * 호출 체인: fio_libpmem_open_file() → [fio_libpmem_file] → libpmem::pmem_map_file
 */
static int fio_libpmem_file(struct thread_data *td, struct fio_file *f,
			    size_t length, off_t off)
{
	struct fio_libpmem_data *fdd = FILE_ENG_DATA(f);  /* [한국어] 파일별 엔진 데이터 획득 */
	mode_t mode = S_IWUSR | S_IRUSR;   /* [한국어] 파일 생성 시 권한(소유자 RW) */
	size_t mapped_len;                 /* [한국어] pmem_map_file이 실제 매핑한 길이 수신 버퍼 */
	int is_pmem;                       /* [한국어] 매핑 대상이 진짜 PMEM DAX인지 여부 수신 */

	dprint(FD_IO, "DEBUG fio_libpmem_file\n");   /* [한국어] 진입 로그 */
	dprint(FD_IO, "f->file_name = %s td->o.verify = %d \n", f->file_name,
			td->o.verify);                        /* [한국어] 대상 파일명 + verify 모드 로그 */
	dprint(FD_IO, "length = %ld f->fd = %d off = %ld file mode = %d \n",
			length, f->fd, off, mode);            /* [한국어] 매핑 파라미터 로그 */

	/* unmap any existing mapping */
	/* [한국어] 이 파일에 이미 매핑이 남아 있으면(재진입/재오픈 경로) 먼저 해제해
	 * 주소 공간 누수와 이중 매핑을 방지한다 */
	if (fdd->libpmem_ptr) {
		dprint(FD_IO,"pmem_unmap \n");
		if (pmem_unmap(fdd->libpmem_ptr, fdd->libpmem_sz) < 0)
			return errno;                   /* [한국어] unmap 실패 → errno 그대로 반환 */
		fdd->libpmem_ptr = NULL;             /* [한국어] 해제 성공 후 포인터 무효화 */
	}

	/* [한국어] pmem_map_file 호출:
	 *  - PMEM_FILE_CREATE: 없으면 파일 생성
	 *  - mode: 생성 시 권한
	 *  - mapped_len/is_pmem: 출력 파라미터
	 * 실패(NULL)면 pmem_errormsg()로 사유를 얻어 fio 에러에 등록 */
	if((fdd->libpmem_ptr = pmem_map_file(f->file_name, length, PMEM_FILE_CREATE, mode, &mapped_len, &is_pmem)) == NULL) {
		td_verror(td, errno, pmem_errormsg());
		goto err;
	}

	/* [한국어] 매핑은 성공했지만 DAX가 아닌 일반 파일이면 경고(PMEM 벤치 의미 없음) */
	if (!is_pmem) {
		td_verror(td, errno, "file_name does not point to persistent memory");
	}

err:
	/* [한국어] 에러 발생 + 매핑이 살아있다면 롤백으로 unmap 수행 */
	if (td->error && fdd->libpmem_ptr)
		pmem_unmap(fdd->libpmem_ptr, length);

	return td->error;  /* [한국어] 누적된 td->error 반환(0이면 성공) */
}

/*
 * [한국어]
 * fio_libpmem_open_file - ioengine_ops.open_file 콜백. fio_file을 PMEM 매핑으로 '오픈'.
 *
 * @td: 잡 컨텍스트.
 * @f:  오픈 대상 fio_file (파일명/io_size 등 선제 설정 완료).
 * @return: 0 성공, 비 0 실패.
 *
 * 이 엔진은 실제 open(2) 대신 pmem_map_file로 매핑을 수립한다. 이미 열린 상태면
 * 먼저 닫고, 새로 fdd를 calloc하여 f에 부착한 뒤 fio_libpmem_file 헬퍼로 매핑한다.
 *
 * 호출 체인: backend → td_io_open_file → ioengine_ops.open_file → [fio_libpmem_open_file]
 */
static int fio_libpmem_open_file(struct thread_data *td, struct fio_file *f)
{
	struct fio_libpmem_data *fdd;  /* [한국어] 새로 할당할 엔진 파일 데이터 */

	dprint(FD_IO, "DEBUG fio_libpmem_open_file\n");             /* [한국어] 진입 로그 */
	dprint(FD_IO, "f->io_size=%ld\n", f->io_size);               /* [한국어] 매핑할 파일 크기 */
	dprint(FD_IO, "td->o.size=%lld\n", td->o.size);              /* [한국어] 잡 요청 총 I/O 크기 */
	dprint(FD_IO, "td->o.iodepth=%d\n", td->o.iodepth);          /* [한국어] 큐 깊이(동기 엔진이라 의미 제한) */
	dprint(FD_IO, "td->o.iodepth_batch=%d\n", td->o.iodepth_batch); /* [한국어] 배치 크기 */

	/* [한국어] 이미 오픈되어 있다면(재시도 경로) 우선 닫는다 — 이중 매핑 방지 */
	if (fio_file_open(f))
		td_io_close_file(td, f);

	/* [한국어] fdd 0초기화 할당 — 실패 시 OOM으로 종료 */
	fdd = calloc(1, sizeof(*fdd));
	if (!fdd) {
		return 1;
	}
	FILE_SET_ENG_DATA(f, fdd);      /* [한국어] 파일→엔진 데이터 슬롯 부착 */
	fdd->libpmem_sz = f->io_size;    /* [한국어] 매핑 크기 = fio가 계산한 파일 I/O 크기 */
	fdd->libpmem_off = 0;            /* [한국어] 부분 매핑 미사용 — 시작 0 */

	/* [한국어] 실제 매핑 수행 후 결과 전파 */
	return fio_libpmem_file(td, f, fdd->libpmem_sz, fdd->libpmem_off);
}

/*
 * [한국어]
 * fio_libpmem_prep - io_u의 파일 오프셋을 매핑된 가상주소로 변환하는 prep 콜백.
 *
 * @td:   잡 컨텍스트.
 * @io_u: 제출 직전 I/O 유닛. offset/buflen이 이미 확정되어 있음.
 * @return: 0 성공, EIO(요청 크기 > 파일 크기).
 *
 * mmap 엔진 계열 공통 기법: io_u->mmap_data = base + offset(파일) - map_off - file_off.
 * 이후 queue()에서 이 주소를 대상으로 memcpy를 수행한다.
 *
 * 호출 체인: td_io_prep → ioengine_ops.prep → [fio_libpmem_prep]
 */
static int fio_libpmem_prep(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;              /* [한국어] 대상 파일 */
	struct fio_libpmem_data *fdd = FILE_ENG_DATA(f); /* [한국어] 파일 매핑 정보 */

	dprint(FD_IO, "DEBUG fio_libpmem_prep\n");    /* [한국어] 진입 로그 */
	dprint(FD_IO, "io_u->offset %llu : fdd->libpmem_off %ld : "
			"io_u->buflen %llu : fdd->libpmem_sz %ld\n",
			io_u->offset, fdd->libpmem_off,
			io_u->buflen, fdd->libpmem_sz);         /* [한국어] 오프셋/크기 덤프 */

	/* [한국어] 블록 크기가 파일 자체 크기보다 크면 매핑 범위 밖이므로 거절 */
	if (io_u->buflen > f->real_file_size) {
		log_err("libpmem: bs bigger than the file size\n");
		return EIO;
	}

	/* [한국어] 매핑 기준 가상주소 계산:
	 *   base(libpmem_ptr) + (파일 내 절대 오프셋 io_u->offset)
	 *   − 매핑 시작 오프셋(libpmem_off, 현재 0)
	 *   − fio가 부여한 파일 시작 오프셋(file_offset)
	 * 결과 mmap_data는 queue()에서 memcpy 대상/출처로 사용된다. */
	io_u->mmap_data = fdd->libpmem_ptr + io_u->offset - fdd->libpmem_off
				- f->file_offset;
	return 0;  /* [한국어] 성공 */
}

/*
 * [한국어]
 * fio_libpmem_queue - ioengine_ops.queue 콜백. I/O 한 건을 즉시(동기) 완료한다.
 *
 * @td:   잡 컨텍스트.
 * @io_u: 제출할 I/O 유닛. prep()에서 mmap_data 세팅 완료.
 * @return: FIO_Q_COMPLETED — 항상 호출 내에서 완료 처리됨을 fio 코어에 알림.
 *
 * 동작:
 *  - READ  : memcpy(xfer_buf ← mmap_data)  — 단순 로드(PMEM 읽기는 시스템콜 없이 즉시).
 *  - WRITE : pmem_memcpy(mmap_data ← xfer_buf, flags) — NT/T, DRAIN 여부를 flags로 제어.
 *  - SYNC/DATASYNC/SYNC_FILE_RANGE: pmem_drain()으로 스토어 버퍼 영속 배리어.
 * 실행 컨텍스트: 잡 스레드. 동기 엔진이라 큐잉 없이 호출 내에서 끝난다.
 *
 * 호출 체인: td_io_queue → ioengine_ops.queue → [fio_libpmem_queue] → libpmem::memcpy/drain
 */
static enum fio_q_status fio_libpmem_queue(struct thread_data *td,
					   struct io_u *io_u)
{
	unsigned flags = 0;   /* [한국어] pmem_memcpy 전달 플래그 비트마스크 */

	fio_ro_check(td, io_u);  /* [한국어] readonly 잡인데 WRITE 시도 시 단속 */
	io_u->error = 0;         /* [한국어] 에러 필드 초기화 */

	dprint(FD_IO, "DEBUG fio_libpmem_queue\n");  /* [한국어] 진입 로그 */
	dprint(FD_IO, "td->o.odirect %d td->o.sync_io %d\n",
			td->o.odirect, td->o.sync_io);        /* [한국어] 현재 동기/다이렉트 플래그 로그 */
	/* map both O_SYNC / DSYNC to not use NODRAIN */
	/* [한국어] sync_io면 DRAIN 강제(플래그 0), 아니면 NODRAIN으로 배치 드레인 지연 */
	flags = td->o.sync_io ? 0 : PMEM_F_MEM_NODRAIN;
	/* [한국어] direct=1 → NONTEMPORAL(캐시 우회 스토어), 아니면 TEMPORAL(일반) */
	flags |= td->o.odirect ? PMEM_F_MEM_NONTEMPORAL : PMEM_F_MEM_TEMPORAL;

	switch (io_u->ddir) {   /* [한국어] 데이터 방향(DDIR)별 분기 */
	case DDIR_READ:
		/* [한국어] PMEM→유저 버퍼로 단순 memcpy. PMEM 읽기는 DRAM처럼 로드-스토어만으로 됨 */
		memcpy(io_u->xfer_buf, io_u->mmap_data, io_u->xfer_buflen);
		break;
	case DDIR_WRITE:
		dprint(FD_IO, "DEBUG mmap_data=%p, xfer_buf=%p\n",
				io_u->mmap_data, io_u->xfer_buf);   /* [한국어] 주소 디버그 */
		/* [한국어] libpmem의 최적화된 memcpy — NT/T, DRAIN 플래그에 따라
		 * CLWB/NTSTORE/SFENCE 등을 자동 삽입하여 영속성을 관리 */
		pmem_memcpy(io_u->mmap_data,
					io_u->xfer_buf,
					io_u->xfer_buflen,
					flags);
		break;
	case DDIR_SYNC:
	case DDIR_DATASYNC:
	case DDIR_SYNC_FILE_RANGE:
		/* [한국어] 드레인: 이전 NT-store들이 실제 PMEM에 영속되도록 배리어 수행 */
		pmem_drain();
		break;
	default:
		io_u->error = EINVAL;  /* [한국어] 지원하지 않는 ddir */
		break;
	}

	return FIO_Q_COMPLETED;  /* [한국어] 동기 완료 — 코어가 바로 put_io_u로 회수 */
}

/*
 * [한국어]
 * fio_libpmem_close_file - 파일 close 콜백. 매핑 해제 + fdd 반환.
 *
 * @td: 잡 컨텍스트.
 * @f:  닫을 fio_file.
 * @return: 0 성공, 그 외 실패.
 *
 * 호출 체인: backend → td_io_close_file → ioengine_ops.close_file → [fio_libpmem_close_file]
 */
static int fio_libpmem_close_file(struct thread_data *td, struct fio_file *f)
{
	struct fio_libpmem_data *fdd = FILE_ENG_DATA(f);  /* [한국어] 부착된 엔진 데이터 */
	int ret = 0;                                       /* [한국어] 반환 누적 */

	dprint(FD_IO, "DEBUG fio_libpmem_close_file\n");   /* [한국어] 진입 로그 */

	/* [한국어] 실제 매핑이 살아있으면 해제 */
	if (fdd->libpmem_ptr)
		ret = pmem_unmap(fdd->libpmem_ptr, fdd->libpmem_sz);
	/* [한국어] FD가 열려 있다면(이 엔진은 보통 mmap만 사용해 FD가 열리지 않을 수도 있음)
	 * 일반 close로 정리. &= 연산으로 에러 누적 */
	if (fio_file_open(f))
		ret &= generic_close_file(td, f);

	FILE_SET_ENG_DATA(f, NULL);  /* [한국어] 슬롯 해제 */
	free(fdd);                   /* [한국어] 엔진 데이터 메모리 반환 */

	return ret;                  /* [한국어] 결과 전파 */
}

/*
 * [한국어] ioengine_ops — libpmem 엔진 vtable.
 * 이 구조체가 fio 코어↔엔진 사이의 공식 계약이며, 필드 하나당 하나의 역할을 갖는다.
 * 누가 설정/읽는지, 계약상 호출 시점, 미설정 시 기본 동작을 각 필드 주석에서 명시.
 * 설정자: 정적 초기화(파일 전역). 읽는 자: fio 코어(backend.c, ioengines.c).
 * 값 범위: FIO_IOOPS_VERSION과 일치해야 하며, 콜백 포인터는 NULL 또는 유효 함수.
 * 동기화: 읽기 전용 구조체 — 초기화 이후 수정되지 않음.
 */
FIO_STATIC struct ioengine_ops ioengine = {
	.name		= "libpmem",
	/* [한국어] 엔진 식별자. ioengine=libpmem 잡 옵션이 이 이름과 매칭된다.
	 * 설정자: 정적 초기화. 읽는 자: load_ioengine→find_ioengine 탐색. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] 엔진 ABI 버전 상수. fio 코어가 자신의 빌드 버전과 비교해
	 * 불일치하면 로드 거부하여 외부 .so 엔진의 ABI 드리프트를 막는다. */

	.init		= fio_libpmem_init,
	/* [한국어] 잡 시작 1회 — 옵션 정합성 검증(페이지 정렬 + fsync 조합 불가).
	 * 호출 시점: td_io_init(). 실패(1) 시 fio 코어가 잡을 중단. */

	.prep		= fio_libpmem_prep,
	/* [한국어] io_u의 offset을 매핑된 가상 주소로 변환해 io_u->mmap_data에 저장.
	 * 호출 시점: get_io_u 이후 queue 직전. mmap 계열 엔진 공통 패턴. */

	.queue		= fio_libpmem_queue,
	/* [한국어] I/O 1건 실행. memcpy/pmem_memcpy/pmem_drain로 동기 완료.
	 * 반환값: FIO_Q_COMPLETED(항상). 동기 엔진이라 commit/getevents 미사용. */

	.open_file	= fio_libpmem_open_file,
	/* [한국어] pmem_map_file로 DAX 파일 매핑. generic_open_file 대신 전용 구현을
	 * 제공하는 이유는 read/write syscall 대신 가상주소 기반 memcpy를 쓰기 때문. */

	.close_file	= fio_libpmem_close_file,
	/* [한국어] pmem_unmap + fdd free. open_file과 짝. */

	.get_file_size	= generic_get_file_size,
	/* [한국어] stat(2) 기반 공통 구현. DAX 파일도 일반 파일시스템 위에 있어 동일. */

	.prepopulate_file = generic_prepopulate_file,
	/* [한국어] fio가 요청 크기만큼 사전 채움(fallocate/write) 수행 시 공통 구현 사용. */

	.flags		= FIO_SYNCIO | FIO_RAWIO | FIO_DISKLESSIO | FIO_NOEXTEND |
				FIO_NODISKUTIL | FIO_BARRIER | FIO_MEMALIGN,
	/* [한국어] 엔진 특성 플래그 비트마스크:
	 *  - FIO_SYNCIO    : 동기 엔진 — queue() 반환 = 완료. fio 코어가 submit/complete를
	 *                    자동 기록하고 commit/getevents 호출을 건너뜀.
	 *  - FIO_RAWIO     : 원시(raw) I/O 의미 — 블록 디바이스처럼 정렬 제약이 있음을 알림.
	 *  - FIO_DISKLESSIO: 실제 디스크 I/O 없음(페이지 캐시·블록 레이어 우회). DAX mmap 모델.
	 *  - FIO_NOEXTEND  : 파일 크기 자동 확장 금지. 매핑은 고정 크기에서 이뤄지므로
	 *                    잡 실행 중 크기 변동이 있으면 SIGBUS 발생 위험.
	 *  - FIO_NODISKUTIL: /proc/diskstats 기반 디스크 사용률 통계 수집 제외.
	 *  - FIO_BARRIER   : pmem_drain이 메모리 배리어 역할을 하므로 fio가 배리어 의미를 추적.
	 *  - FIO_MEMALIGN  : io_u 버퍼 메모리 정렬 요구(NT-store에 필요한 정렬 수호). */
};

/*
 * [한국어]
 * fio_libpmem_register - 라이브러리 로드시(.fio_init 생성자)에 ioengine을 전역 등록.
 * 실행 컨텍스트: 프로세스 시작 시 단 1회 자동 호출.
 */
static void fio_init fio_libpmem_register(void)
{
	register_ioengine(&ioengine);  /* [한국어] fio 코어 엔진 리스트에 추가 */
}

/*
 * [한국어]
 * fio_libpmem_unregister - 프로세스 종료시(.fio_exit 소멸자)에 엔진 등록 해제.
 */
static void fio_exit fio_libpmem_unregister(void)
{
	unregister_ioengine(&ioengine);  /* [한국어] fio 코어 엔진 리스트에서 제거 */
}
