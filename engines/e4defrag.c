/*
 * [한국어 설명] e4defrag I/O 엔진 구현 (e4defrag.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 ext4의 온라인 조각 모음(defragmentation) 경로를 벤치마크하기 위한
 * fio I/O 엔진이다. EXT4_IOC_MOVE_EXT ioctl을 사용해 "도너(donor)" 파일의
 * 논리 블록을 대상 "원본(orig)" 파일의 블록 위치로 이동시키는 동작을 반복
 * 수행함으로써, ext4의 extent 교체 로직과 저널 영향, 블록 할당자 부담을 측정한다.
 * 실제 데이터 생성은 READ/TRIM 방향에서는 거부되며 DDIR_WRITE만 의미를 가진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 엔진 플러그인 계약에 따라 td_io_init→fio_e4defrag_init에서 도너 파일을
 * 준비하고, td_io_queue 시점에 fio_e4defrag_queue가 각 io_u에 대해 ioctl을
 * 수행한다. 동기 엔진이므로 queue 단계에서 바로 FIO_Q_COMPLETED. 실행 컨텍스트는
 * 잡 스레드(호스트 유저스페이스), 커널 측은 ext4의 ioctl 핸들러(fs/ext4/move_extent.c)
 * 경로로 진입해 extent tree를 교체한다. 블록 크기(st_blksize)는 fstat으로 얻어
 * ioctl의 블록 단위 인자를 계산하는 기준이 된다.
 *
 * === 타 모듈과의 연결 ===
 * - 상위: ioengines.c의 td_io_init/td_io_queue/td_io_cleanup 훅에서 호출.
 * - 하위: ext4 ioctl(EXT4_IOC_MOVE_EXT), open(2)/close(2)/fallocate(2)/fstat(2)/
 *   ftruncate(2) 시스템 호출. sync 위임 없음(이 엔진은 sync 방향을 처리하지 않음).
 * - 옵션 시스템: optgroup.h/파서(FIO_OPT_C_ENGINE/FIO_OPT_G_E4DEFRAG)와 연계하여
 *   잡 파일의 donorname/inplace 옵션을 파싱해 e4defrag_options로 전달.
 * - 공유 자료구조: thread_data::io_ops_data에 e4defrag_data(도너 fd, blksize)를 저장.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_e4defrag_init(): 도너 파일 생성/오픈, 공간 사전 할당(inplace=0), blksize 취득.
 * - fio_e4defrag_cleanup(): 도너 파일 닫기 및 e4defrag_data 해제.
 * - fio_e4defrag_queue(): move_extent 구조체를 채워 EXT4_IOC_MOVE_EXT ioctl 수행.
 *   부분 이동 시 io_u->resid로 잔량 보고.
 * - struct e4defrag_data: 런타임 상태(도너 fd, FS 블록 크기).
 * - struct e4defrag_options: 잡 옵션(donor_name, inplace 토글).
 * - struct move_extent: ext4 UAPI가 정의하는 ioctl 인자 레이아웃.
 */

/*
 * ioe_e4defrag:  ioengine for https://git.kernel.org/pub/scm/linux/kernel/git/axboe/fio
 *
 * IO engine that does regular EXT4_IOC_MOVE_EXT ioctls to simulate
 * defragment activity
 *
 */

#include <sys/types.h>     /* [한국어] __u32/__u64 등 ioctl UAPI가 사용하는 기본 타입을 위해. */
#include <sys/stat.h>      /* [한국어] fstat(2)/struct stat — 블록 크기(st_blksize) 조회. */
#include <stdio.h>         /* [한국어] sprintf로 도너 경로 조립 시 사용. */
#include <errno.h>         /* [한국어] 각 syscall 실패 경로에서 errno 전달. */
#include <fcntl.h>         /* [한국어] open(2) 플래그(O_CREAT/O_WRONLY), fallocate(2). */
#include <stdint.h>        /* [한국어] uint 계열 명시 타입. */

#include "../fio.h"        /* [한국어] fio 핵심 타입/헬퍼. */
#include "../optgroup.h"   /* [한국어] FIO_OPT_C_ENGINE/FIO_OPT_G_E4DEFRAG 카테고리/그룹 상수. */

/*
 * [한국어] EXT4_IOC_MOVE_EXT — ext4 UAPI 사양의 ioctl 번호.
 * 커널 헤더에 정의가 없을 경우를 대비해 폴백으로 재정의.
 * _IOWR('f', 15, ...)는 매직 'f'(ext) + 15 + IN/OUT 방향을 의미.
 * 설정자: 정적 매크로. 읽는 자: fio_e4defrag_queue의 ioctl 호출.
 */
#ifndef EXT4_IOC_MOVE_EXT
#define EXT4_IOC_MOVE_EXT               _IOWR('f', 15, struct move_extent)
/*
 * [한국어] struct move_extent — EXT4_IOC_MOVE_EXT의 인자 레이아웃(UAPI).
 * 이 구조체는 커널과 동일 레이아웃이어야 하며, 필드 순서를 바꾸지 않는다.
 */
struct move_extent {
	__u32 reserved;
	/* [한국어] 예약 필드. 반드시 0. 설정자: memset으로 0 초기화.
	 * 읽는 자: 커널. 값 범위: 0. */

	__u32 donor_fd;
	/* [한국어] 블록 공급 원본이 될 도너 파일의 fd.
	 * 설정자: queue()에서 ed->donor_fd로 채움.
	 * 읽는 자: ext4 ioctl 핸들러. 값 범위: 유효 fd. */

	__u64 orig_start;
	/* [한국어] orig 파일에서 블록 단위의 시작 오프셋 (바이트/blksize).
	 * 설정자: io_u->offset/bsz. 값 범위: 0..파일크기/bsz. */

	__u64 donor_start;
	/* [한국어] 도너 파일에서 블록 단위의 시작 오프셋. 본 구현은 orig_start와 동일 값 사용. */

	__u64 len;
	/* [한국어] 이동할 블록 수. (offset+len)의 블록 경계 올림에서 orig_start를 뺀 값. */

	__u64 moved_len;
	/* [한국어] 커널이 반환하는 실제 이동된 블록 수.
	 * 설정자: 커널. 읽는 자: queue() — 부분 이동 시 io_u->resid 계산에 사용. */
};
#endif

/*
 * [한국어] struct e4defrag_data — 잡(thread_data) 한 개의 런타임 상태.
 * thread_data::io_ops_data 슬롯에 포인터가 저장된다.
 */
struct e4defrag_data {
	int donor_fd;
	/* [한국어] 도너 파일 디스크립터. init에서 open되고 cleanup에서 close.
	 * 설정자: fio_e4defrag_init. 읽는 자: fio_e4defrag_queue(매 I/O).
	 * 값 범위: 유효 fd(>=0) 또는 -1(미오픈).
	 * 동기화: 한 잡 스레드만 접근(엔진 계약상 잡당 단일 스레드). */

	int bsz;
	/* [한국어] 도너 파일이 위치한 FS의 블록 크기(byte). fstat의 st_blksize.
	 * 설정자: init에서 한 번 설정. 읽는 자: queue가 move_extent의 블록 단위
	 * 오프셋/길이 계산 시 나눗셈 기준. 값 범위: 보통 4096(페이지) 또는 FS 기본값.
	 * 동기화: 초기화 후 불변이므로 별도 락 불필요. */
};

/*
 * [한국어] struct e4defrag_options — 잡 옵션. fio 옵션 파서가 채워서 td->eo에 저장.
 */
struct e4defrag_options {
	void *pad;
	/* [한국어] 옵션 구조체 선두 패딩. fio 옵션 파서(FIO_OPT_*)의 off1 오프셋
	 * 기준을 맞추기 위해 관례적으로 선두에 둔다. 설정자/읽는 자: 없음(플레이스홀더). */

	unsigned int inplace;
	/* [한국어] 1=매 I/O마다 도너 공간을 fallocate하고 완료 후 ftruncate로 비움.
	 * 0=init에서 한 번만 fallocate. 설정자: 옵션 파서. 읽는 자: init/queue.
	 * 값 범위: 0 또는 1(maxval=1). */

	char *donor_name;
	/* [한국어] 도너 파일 경로(상대/절대). 필수 옵션.
	 * 설정자: 옵션 파서(FIO_OPT_STR_STORE). 읽는 자: init.
	 * 값 범위: NUL 종료 문자열. NULL/빈 문자열이면 init 실패. */
};

static struct fio_option options[] = {
	{
		.name	= "donorname",
		.lname	= "Donor Name",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct e4defrag_options, donor_name),
		.help	= "File used as a block donor",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_E4DEFRAG,
	},
	{
		.name	= "inplace",
		.lname	= "In Place",
		.type	= FIO_OPT_INT,
		.off1	= offsetof(struct e4defrag_options, inplace),
		.minval	= 0,
		.maxval	= 1,
		.help	= "Alloc and free space inside defrag event",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_E4DEFRAG,
	},
	{
		.name	= NULL,
	},
};

/*
 * [한국어]
 * fio_e4defrag_init - e4defrag 엔진 초기화 콜백
 *
 * @td: 잡 컨텍스트. td->eo(e4defrag_options), td->o.directory/start_offset/
 *      file_size_high를 입력으로 사용하고, td->io_ops_data에 e4defrag_data 포인터를 저장.
 * @return: 0=성공, 1=실패(도너 옵션 누락/open 실패/fallocate 실패/fstat 실패).
 *
 * 도너 파일을 준비한다: (1) donorname 옵션 검증, (2) 디렉터리 접두사 붙여 경로 구성,
 * (3) O_CREAT|O_WRONLY로 오픈, (4) inplace=0일 때 [start_offset, file_size_high]
 * 구간 공간을 fallocate로 선점, (5) fstat으로 FS 블록 크기를 취득해 ed->bsz 저장.
 *
 * 실행 컨텍스트: 잡 시작 시 각 잡 스레드에서 1회 호출.
 *
 * 호출 체인: td_io_init() → [fio_e4defrag_init] → open/fallocate/fstat
 */
static int fio_e4defrag_init(struct thread_data *td)
{
	int r, len = 0;                              /* [한국어] r: syscall 반환값 임시. len: 디렉터리 접두사 길이(이후 파일명 이어붙이는 오프셋). */
	struct e4defrag_options *o = td->eo;         /* [한국어] 잡의 엔진 옵션 포인터. 파서가 미리 채웠다. */
	struct e4defrag_data *ed;                    /* [한국어] 새로 할당할 런타임 상태. */
	struct stat stub;                            /* [한국어] fstat 결과 수령용 임시. */
	char donor_name[PATH_MAX];                   /* [한국어] 경로 조립 버퍼. PATH_MAX는 시스템 상한. */

	if (!o->donor_name || !strlen(o->donor_name)) {  /* [한국어] 필수 옵션 누락 검증. NULL 또는 빈 문자열이면 사용자 오류. */
		log_err("'donorname' options required\n");
		return 1;                                /* [한국어] 초기화 실패 — 잡이 시작되지 않음. */
	}

	ed = calloc(1, sizeof(*ed));                 /* [한국어] 0으로 초기화된 상태 구조체 할당. */
	if (!ed) {
		td_verror(td, ENOMEM, "io_queue_init"); /* [한국어] 메모리 부족 보고. */
		return 1;
	}

	if (td->o.directory)                                         /* [한국어] 잡이 디렉터리 prefix를 지정했다면 */
		len = sprintf(donor_name, "%s/", td->o.directory);   /* [한국어] "dir/" 조립 후 이어붙일 오프셋 반환. */
	sprintf(donor_name + len, "%s", o->donor_name);              /* [한국어] 그 뒤에 실제 도너 파일명 append. */

	ed->donor_fd = open(donor_name, O_CREAT|O_WRONLY, 0644);     /* [한국어] 도너 파일 생성/오픈. 쓰기 전용이면 충분(ioctl은 별도 권한 검사). */
	if (ed->donor_fd < 0) {                                      /* [한국어] open 실패 경로. */
		td_verror(td, errno, "io_queue_init");
		log_err("Can't open donor file %s err:%d\n", donor_name, ed->donor_fd);
		free(ed);                                           /* [한국어] 누수 방지. */
		return 1;
	}

	if (!o->inplace) {                                           /* [한국어] non-inplace: 잡 전체 구간을 미리 fallocate 해 둠. */
		long long __len = td->o.file_size_high - td->o.start_offset;  /* [한국어] 잡 범위 크기(바이트). */
		r = fallocate(ed->donor_fd, 0, td->o.start_offset, __len);    /* [한국어] flags=0 → 실제 블록 할당. MOVE_EXT가 쓸 블록 공급. */
		if (r)
			goto err;                                    /* [한국어] 실패 시 정리 경로로 이동. */
	}
	r = fstat(ed->donor_fd, &stub);                              /* [한국어] 블록 크기(st_blksize) 조회. ioctl의 블록 단위 환산 기준. */
	if (r)
		goto err;

	ed->bsz = stub.st_blksize;                                   /* [한국어] 런타임 상태에 저장. */
	td->io_ops_data = ed;                                        /* [한국어] 엔진 간 통신 슬롯에 연결. 이후 queue/cleanup이 꺼내 쓴다. */
	return 0;                                                    /* [한국어] 초기화 성공. */
err:
	td_verror(td, errno, "io_queue_init");                       /* [한국어] 마지막 errno로 에러 보고. */
	close(ed->donor_fd);                                         /* [한국어] 열었던 fd 정리. */
	free(ed);
	return 1;
}

/*
 * [한국어]
 * fio_e4defrag_cleanup - 엔진 정리 콜백
 *
 * @td: 잡 컨텍스트. td->io_ops_data에서 e4defrag_data를 꺼내 자원 해제.
 *
 * 호출 체인: td_io_cleanup() → [fio_e4defrag_cleanup] → close(2)/free(3)
 *
 * 실행 컨텍스트: 잡 종료 시 1회 호출. init가 성공하지 못했어도 fio가 호출할 수
 * 있으므로 NULL 체크를 수행한다.
 */
static void fio_e4defrag_cleanup(struct thread_data *td)
{
	struct e4defrag_data *ed = td->io_ops_data;   /* [한국어] init이 저장한 상태 포인터 취득. */
	if (ed) {                                     /* [한국어] init 실패 시 NULL일 수 있어 방어. */
		if (ed->donor_fd >= 0)                /* [한국어] 유효 fd일 때만 close. */
			close(ed->donor_fd);
		free(ed);                             /* [한국어] 상태 메모리 해제. */
	}
}


/*
 * [한국어]
 * fio_e4defrag_queue - e4defrag 엔진의 I/O 제출 콜백
 *
 * @td:   잡 컨텍스트. td->io_ops_data에서 ed, td->eo에서 옵션을 취득.
 * @io_u: 처리할 I/O 유닛. file/offset/xfer_buflen/ddir 사용, error/resid 기록.
 * @return: 항상 FIO_Q_COMPLETED.
 *
 * DDIR_WRITE에서만 유효. move_extent를 채워 EXT4_IOC_MOVE_EXT ioctl 실행.
 * inplace=1이면 매 I/O마다 도너 공간 fallocate, 완료 후 ftruncate로 비움.
 * 부분 이동 시 moved_len*bsz 바이트만 반영하고 나머지는 io_u->resid로 보고.
 *
 * 실행 컨텍스트: 잡 스레드.
 *
 * 호출 체인: td_io_queue() → [fio_e4defrag_queue] → fallocate/ioctl/ftruncate
 */
static enum fio_q_status fio_e4defrag_queue(struct thread_data *td,
					    struct io_u *io_u)
{

	int ret;                                         /* [한국어] syscall/ioctl 반환값. */
	unsigned long long len;                          /* [한국어] 블록 단위 계산 중간값 및 이동 바이트. */
	struct move_extent me;                           /* [한국어] ioctl 인자 구조체 인스턴스. */
	struct fio_file *f = io_u->file;                 /* [한국어] orig 파일(이동 대상). */
	struct e4defrag_data *ed = td->io_ops_data;      /* [한국어] init이 준비한 런타임 상태. */
	struct e4defrag_options *o = td->eo;             /* [한국어] 잡 옵션(inplace 판정). */

	fio_ro_check(td, io_u);                          /* [한국어] read-only 잡이 write를 시도하는 설정 오류 방어. */

	/* Theoretically defragmentation should not change data, but it
	 * changes data layout. So this function handle only DDIR_WRITE
	 * in order to satisfy strict read only access pattern
	 */
	if (io_u->ddir != DDIR_WRITE) {                  /* [한국어] READ/TRIM은 데이터 레이아웃 변경을 유발해 read-only 의미와 충돌. 거부. */
		io_u->error = EINVAL;
		return FIO_Q_COMPLETED;
	}

	if (o->inplace) {                                /* [한국어] inplace 모드: 매 I/O마다 도너 공간을 새로 준비. */
		ret = fallocate(ed->donor_fd, 0, io_u->offset, io_u->xfer_buflen);  /* [한국어] 해당 구간에 블록 할당. */
		if (ret)
			goto out;                        /* [한국어] 실패 시 곧바로 에러 경로. */
	}

	memset(&me, 0, sizeof(me));                      /* [한국어] reserved 포함 전체 0으로 초기화(커널 요구사항). */
	me.donor_fd = ed->donor_fd;                      /* [한국어] 블록 공급자. */
	me.orig_start = io_u->offset / ed->bsz;          /* [한국어] 바이트→블록 변환. ioctl은 블록 단위. */
	me.donor_start = me.orig_start;                  /* [한국어] 같은 블록 위치에서 교체(단순 1:1 매핑 정책). */
	len = (io_u->offset + io_u->xfer_buflen + ed->bsz -1);  /* [한국어] 끝 바이트를 블록 경계로 올림 계산. */
	me.len = len / ed->bsz - me.orig_start;          /* [한국어] 이동할 블록 수 = 올림(end/bsz) - 시작 블록. */

	ret = ioctl(f->fd, EXT4_IOC_MOVE_EXT, &me);      /* [한국어] 커널 ext4 move_extent 실행. 성공 시 0, 실패 시 -1/errno. */
	len = me.moved_len * ed->bsz;                    /* [한국어] 커널이 실제 이동한 블록 수를 바이트로 환산. */

	if (len > io_u->xfer_buflen)                     /* [한국어] 경계 올림으로 요청보다 많을 수 있어 clamp. */
		len = io_u->xfer_buflen;

	if (len != io_u->xfer_buflen) {                  /* [한국어] 부분 이동 발생. */
		if (len) {
			io_u->resid = io_u->xfer_buflen - len;  /* [한국어] 잔량을 fio 통계에 보고. */
			io_u->error = 0;                        /* [한국어] 부분 성공은 에러 아님. */
		} else {
			/* access beyond i_size */
			io_u->error = EINVAL;                   /* [한국어] 전혀 이동하지 못함 — 파일 경계 초과. */
		}
	}
	if (ret)
		io_u->error = errno;                        /* [한국어] ioctl 자체 실패는 errno 반영. */

	if (o->inplace)
		ret = ftruncate(ed->donor_fd, 0);           /* [한국어] inplace: 다음 I/O를 위해 도너 초기화. */
out:
	if (ret && !io_u->error)
		io_u->error = errno;                        /* [한국어] 에러 수집이 아직 안 된 실패 경로 커버. */

	return FIO_Q_COMPLETED;                         /* [한국어] 동기 완료 신호. */
}

static struct ioengine_ops ioengine = {
	.name			= "e4defrag",
	.version		= FIO_IOOPS_VERSION,
	.init			= fio_e4defrag_init,
	.queue			= fio_e4defrag_queue,
	.open_file		= generic_open_file,
	.close_file		= generic_close_file,
	.get_file_size		= generic_get_file_size,
	.flags			= FIO_SYNCIO,
	.cleanup		= fio_e4defrag_cleanup,
	.options		= options,
	.option_struct_size	= sizeof(struct e4defrag_options),

};

static void fio_init fio_syncio_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_syncio_unregister(void)
{
	unregister_ioengine(&ioengine);
}
