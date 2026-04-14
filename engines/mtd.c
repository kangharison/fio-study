/*
 * MTD engine
 *
 * IO engine that reads/writes from MTD character devices.
 *
 */

/*
 * [한국어 설명] MTD(Memory Technology Device) I/O 엔진 구현 (mtd.c)
 *
 * === 파일의 역할 ===
 * /dev/mtdN 캐릭터 디바이스를 대상으로 NOR/NAND 플래시의 실제 read/write/erase를
 * 수행하는 fio I/O 엔진이다. libmtd(fio 내장 복제본: oslib/libmtd.c)를 통해
 * 디바이스 정보 조회/불량 블록 검사/마킹/페이지 단위 기록/블록 erase를 추상화하며,
 * I/O는 항상 erase block(eb) 경계로 쪼개 제출된다. TRIM(DDIR_TRIM)은 erase로 매핑되고,
 * 정렬 위배 시 EINVAL로 실패한다. skip_bad=1 옵션으로 알려진 bad block을 건너뛴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_backend의 잡 루프가 load_ioengine("mtd") 후 open_file→queue(→완료)→close_file 순으로
 * 이 엔진의 콜백을 호출한다. 동기 엔진(FIO_SYNCIO)이라 queue()가 곧 완료까지 수행한다.
 * 실행 컨텍스트는 각 잡 스레드이며, libmtd는 ioctl(/dev/mtdN) 래핑으로 커널 MTD 서브시스템과
 * 통신한다. libmtd_open으로 얻는 전역 desc 핸들은 프로세스 전체에서 공유된다.
 *
 * === 타 모듈과의 연결 ===
 * - fio.h: thread_data, io_u, ioengine_ops, fio_file, td_verror 등.
 * - optgroup.h, FIO_OPT_G_MTD: 엔진 전용 옵션 그룹 등록.
 * - oslib/libmtd.h: mtd_read/write/erase/is_bad/mark_bad/get_dev_info 등.
 * - mtd/mtd-user.h: 커널 MTD UAPI 구조체(mtd_info_user 등, libmtd 내부에서 사용).
 * - 공유 상태: 전역 `desc`(libmtd 핸들)는 등록 시 열려 모든 잡이 공유; 파일별 fmd는
 *   FILE_ENG_DATA(f)로 부착되어 해당 잡 스레드만 접근.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_mtd_data:    파일별 mtd_dev_info 캐시(eb 크기/페이지/oob 등).
 * - struct fio_mtd_options: skip_bad 같은 엔진 전용 옵션.
 * - fio_mtd_queue():        erase block 단위 루프로 read/write/erase 호출.
 * - fio_mtd_is_bad/maybe_mark_bad(): bad block 검사/자동 마킹.
 * - fio_mtd_open_file/close_file/get_file_size(): libmtd 장치 정보 획득·해제.
 */

#include <stdio.h>            /* [한국어] 로그 출력 */
#include <stdlib.h>           /* [한국어] calloc/free */
#include <errno.h>            /* [한국어] errno, EIO/EINVAL/ENOTSUP */
#include <sys/ioctl.h>        /* [한국어] libmtd 내부 ioctl 호출을 위한 헤더 */
#include <mtd/mtd-user.h>     /* [한국어] 커널 MTD UAPI(MEMGETINFO 등) */

#include "../fio.h"           /* [한국어] fio 코어 타입 */
#include "../optgroup.h"      /* [한국어] FIO_OPT_G_* 그룹 매크로 */
#include "../oslib/libmtd.h"  /* [한국어] libmtd 래퍼 API */

/* [한국어] libmtd 프로세스 전역 핸들.
 * 설정자: fio_mtd_register() 생성자. 읽는 자: open_file/queue/close_file 전반.
 * 값 범위: 유효 포인터(등록 이후) 또는 NULL(등록 해제 후).
 * 동기화: libmtd 내부 구현 의존. fio는 단순 핸들 공유. */
static libmtd_t desc;

/*
 * [한국어] MTD 엔진의 파일별 상태.
 * FILE_SET_ENG_DATA(f, fmd)로 부착되어 해당 잡 스레드만 접근.
 */
struct fio_mtd_data {
	struct mtd_dev_info info;
	/* [한국어] MTD 디바이스 메타데이터 캐시(erase block 크기/페이지 크기/oob/전체 크기/타입 등).
	 * 설정자: open_file에서 mtd_get_dev_info() 결과로 채움.
	 * 읽는 자: queue()의 eb/eb_offs/len 계산, fio_mtd_is_bad/mark_bad.
	 * 값 범위: mtd-user UAPI 규정.
	 * 동기화: 파일 1:1 잡 스레드 소유, 읽기 전용 사용. */
};

/*
 * [한국어] 엔진 전용 옵션 저장 구조체(td->eo가 가리킴).
 */
struct fio_mtd_options {
	void *pad; /* avoid off1 == 0 */
	/* [한국어] fio 옵션 시스템은 off1==0을 예약값으로 사용하므로 더미 패딩 필드를 둠.
	 * 설정자: 없음(단순 자리). 읽는 자: 없음. */

	unsigned int skip_bad;
	/* [한국어] 1이면 알려진 bad block을 건너뛴다. 0이면 그대로 접근(에러로 드러남).
	 * 설정자: CLI/잡파일 파서가 options[] 엔트리로 파싱.
	 * 읽는 자: fio_mtd_queue()가 블록마다 검사.
	 * 값 범위: 0/1. 동기화: 불변(잡 시작 후). */
};

/* [한국어] 엔진 전용 옵션 테이블. NULL 엔트리로 종단. */
static struct fio_option options[] = {
	{
		.name	= "skip_bad",
		.lname	= "Skip operations against bad blocks",
		.type	= FIO_OPT_BOOL,
		.off1	= offsetof(struct fio_mtd_options, skip_bad),
		.help	= "Skip operations against known bad blocks.",
		.hide	= 1,
		.def	= "0",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_MTD,
	},
	{
		.name	= NULL,
	},
};

/*
 * [한국어]
 * fio_mtd_maybe_mark_bad - 직전 mtd_* 호출이 EIO로 실패했다면 해당 erase block을
 *                         불량으로 자동 마킹한다.
 * @td, @fmd: 컨텍스트. @io_u: 실패한 요청. @eb: 실패한 erase block 번호.
 * @return: 0 성공(또는 EIO 아님), -1 마킹 자체가 실패.
 * 호출 체인: fio_mtd_queue(실패 분기) → [fio_mtd_maybe_mark_bad] → libmtd::mtd_mark_bad.
 */
static int fio_mtd_maybe_mark_bad(struct thread_data *td,
				  struct fio_mtd_data *fmd,
				  struct io_u *io_u, int eb)
{
	int ret;
	if (errno == EIO) {   /* [한국어] 하드 I/O 에러만 bad로 판단(다른 에러는 일반 실패) */
		ret = mtd_mark_bad(&fmd->info, io_u->file->fd, eb);
		if (ret != 0) {
			/* [한국어] 마킹 자체 실패 → io_u에 에러 기록 + 잡 에러 전파 */
			io_u->error = errno;
			td_verror(td, errno, "mtd_mark_bad");
			return -1;
		}
	}
	return 0;  /* [한국어] EIO 아니거나 마킹 성공 */
}

/*
 * [한국어]
 * fio_mtd_is_bad - skip_bad 모드에서 블록이 불량인지 조회.
 * @return: 0 정상, 1 불량(에러 EIO 세팅), -1 조회 자체 실패.
 * 호출 체인: fio_mtd_queue → [fio_mtd_is_bad] → libmtd::mtd_is_bad.
 */
static int fio_mtd_is_bad(struct thread_data *td,
			  struct fio_mtd_data *fmd,
			  struct io_u *io_u, int eb)
{
	int ret = mtd_is_bad(&fmd->info, io_u->file->fd, eb);   /* [한국어] 상태 조회 */
	if (ret == -1) {
		io_u->error = errno;                     /* [한국어] 조회 실패 기록 */
		td_verror(td, errno, "mtd_is_bad");
	} else if (ret == 1)
		io_u->error = EIO;	/* Silent failure--don't flood stderr */
		/* [한국어] 이미 알려진 불량 — stderr 범람 방지 위해 조용히 EIO 처리 */
	return ret;
}

/*
 * [한국어]
 * fio_mtd_queue - MTD 엔진의 I/O 제출 콜백
 *
 * I/O를 erase block 단위로 분할하여 mtd_read()/mtd_write()/mtd_erase()를 수행한다.
 * skip_bad 옵션이 설정되면 불량 블록을 건너뛰고 다음 블록에서 계속한다.
 * 불량 블록을 발견하면 mtd_mark_bad()로 마킹할 수 있다.
 *
 * 호출 체인: td_io_queue() → [이 함수] → mtd_read()/mtd_write()/mtd_erase()
 */
static enum fio_q_status fio_mtd_queue(struct thread_data *td,
				       struct io_u *io_u)
{
	struct fio_file *f = io_u->file;              /* [한국어] 대상 파일 */
	struct fio_mtd_data *fmd = FILE_ENG_DATA(f);   /* [한국어] 장치 정보 */
	struct fio_mtd_options *o = td->eo;            /* [한국어] 엔진 옵션(skip_bad 등) */
	int local_offs = 0;                             /* [한국어] io_u 버퍼 내 현재 처리 오프셋 */
	int ret;

	fio_ro_check(td, io_u);  /* [한국어] readonly 잡에 WRITE/TRIM 오는지 단속 */

	/*
	 * Errors tend to pertain to particular erase blocks, so divide up
	 * I/O to erase block size.
	 * If an error is encountered, log it and keep going onto the next
	 * block because the error probably just pertains to that block.
	 * TODO(dehrenberg): Divide up reads and writes into page-sized
	 * operations to get more fine-grained information about errors.
	 */
	/* [한국어] io_u 크기가 여러 erase block에 걸칠 수 있으므로 eb 단위로 분할 루프 */
	while (local_offs < io_u->buflen) {
		int eb = (io_u->offset + local_offs) / fmd->info.eb_size;      /* [한국어] 현재 eb 번호 */
		int eb_offs = (io_u->offset + local_offs) % fmd->info.eb_size; /* [한국어] eb 내부 오프셋 */
		/* The length is the smaller of the length remaining in the
		 * buffer and the distance to the end of the erase block */
		/* [한국어] 이번 iteration에서 처리할 길이 — 버퍼 잔여와 eb 잔여 중 작은 쪽 */
		int len = min((int)io_u->buflen - local_offs,
			      (int)fmd->info.eb_size - eb_offs);
		char *buf = ((char *)io_u->buf) + local_offs;                   /* [한국어] 버퍼 포인터 전진 */

		if (o->skip_bad) {
			ret = fio_mtd_is_bad(td, fmd, io_u, eb);
			if (ret == -1)
				break;        /* [한국어] 조회 실패 → 전체 루프 중단 */
			else if (ret == 1)
				goto next;    /* [한국어] 불량 블록 — 조용히 스킵 */
		}
		if (io_u->ddir == DDIR_READ) {
			/* [한국어] 페이지 읽기: libmtd가 내부에서 ECC 반영한 데이터 반환 */
			ret = mtd_read(&fmd->info, f->fd, eb, eb_offs, buf, len);
			if (ret != 0) {
				io_u->error = errno;
				td_verror(td, errno, "mtd_read");
				if (fio_mtd_maybe_mark_bad(td, fmd, io_u, eb))
					break;
			}
		} else if (io_u->ddir == DDIR_WRITE) {
			/* [한국어] 페이지 쓰기(NULL oob/0 mode = 기본) */
			ret = mtd_write(desc, &fmd->info, f->fd, eb,
					    eb_offs, buf, len, NULL, 0, 0);
			if (ret != 0) {
				io_u->error = errno;
				td_verror(td, errno, "mtd_write");
				if (fio_mtd_maybe_mark_bad(td, fmd, io_u, eb))
					break;
			}
		} else if (io_u->ddir == DDIR_TRIM) {
			/* [한국어] MTD erase는 반드시 eb 경계+크기여야 함 — 그 외는 EINVAL */
			if (eb_offs != 0 || len != fmd->info.eb_size) {
				io_u->error = EINVAL;
				td_verror(td, EINVAL,
					  "trim on MTD must be erase block-aligned");
			}
			ret = mtd_erase(desc, &fmd->info, f->fd, eb);   /* [한국어] 블록 erase 수행 */
			if (ret != 0) {
				io_u->error = errno;
				td_verror(td, errno, "mtd_erase");
				if (fio_mtd_maybe_mark_bad(td, fmd, io_u, eb))
					break;
			}
		} else {
			/* [한국어] 그 외 ddir(SYNC 등)은 MTD에서 의미 없음 */
			io_u->error = ENOTSUP;
			td_verror(td, io_u->error, "operation not supported on mtd");
		}

next:
		local_offs += len;  /* [한국어] 다음 블록 조각으로 전진 */
	}

	return FIO_Q_COMPLETED;   /* [한국어] 동기 엔진 — 호출 내 완료 */
}

/*
 * [한국어]
 * fio_mtd_open_file - MTD 장치 오픈 + fmd 생성 + 장치 정보 조회.
 * 호출 체인: backend → td_io_open_file → [fio_mtd_open_file] → generic_open_file/mtd_get_dev_info.
 */
static int fio_mtd_open_file(struct thread_data *td, struct fio_file *f)
{
	struct fio_mtd_data *fmd;
	int ret;

	ret = generic_open_file(td, f);   /* [한국어] 표준 경로로 FD 확보 */
	if (ret)
		return ret;

	fmd = calloc(1, sizeof(*fmd));     /* [한국어] 파일별 상태 할당 */
	if (!fmd)
		goto err_close;

	ret = mtd_get_dev_info(desc, f->file_name, &fmd->info);  /* [한국어] MEMGETINFO 등 */
	if (ret != 0) {
		td_verror(td, errno, "mtd_get_dev_info");
		goto err_free;
	}

	FILE_SET_ENG_DATA(f, fmd);   /* [한국어] 파일→엔진 데이터 슬롯 부착 */
	return 0;

err_free:
	free(fmd);                     /* [한국어] 정보 조회 실패 롤백 */
err_close:
	{
		int fio_unused __ret;
		__ret = generic_close_file(td, f);   /* [한국어] FD 정리(에러 무시) */
		return 1;
	}
}

/*
 * [한국어]
 * fio_mtd_close_file - fmd 해제 후 표준 close 위임.
 */
static int fio_mtd_close_file(struct thread_data *td, struct fio_file *f)
{
	struct fio_mtd_data *fmd = FILE_ENG_DATA(f);

	FILE_SET_ENG_DATA(f, NULL);   /* [한국어] 슬롯 비우기 */
	free(fmd);                      /* [한국어] 장치 정보 캐시 해제 */

	return generic_close_file(td, f);  /* [한국어] FD close */
}

/*
 * [한국어]
 * fio_mtd_get_file_size - 코어에 MTD 장치 size 보고.
 */
static int fio_mtd_get_file_size(struct thread_data *td, struct fio_file *f)
{
	struct mtd_dev_info info;

	int ret = mtd_get_dev_info(desc, f->file_name, &info);
	if (ret != 0) {
		td_verror(td, errno, "mtd_get_dev_info");
		return errno;
	}
	f->real_file_size = info.size;   /* [한국어] fio 코어가 I/O 범위 계산에 사용 */

	return 0;
}

static struct ioengine_ops ioengine = {
	.name		= "mtd",
	.version	= FIO_IOOPS_VERSION,
	.queue		= fio_mtd_queue,
	.open_file	= fio_mtd_open_file,
	.close_file	= fio_mtd_close_file,
	.get_file_size	= fio_mtd_get_file_size,
	.flags		= FIO_SYNCIO | FIO_NOEXTEND,
	.options	= options,
	.option_struct_size	= sizeof(struct fio_mtd_options),
};

/* [한국어] 생성자: libmtd 핸들 열고 엔진 등록 */
static void fio_init fio_mtd_register(void)
{
	desc = libmtd_open();           /* [한국어] 프로세스 전역 libmtd 컨텍스트 개방 */
	register_ioengine(&ioengine);   /* [한국어] fio 엔진 레지스트리에 추가 */
}

/* [한국어] 소멸자: 등록 해제 + libmtd 핸들 정리 */
static void fio_exit fio_mtd_unregister(void)
{
	unregister_ioengine(&ioengine);
	libmtd_close(desc);
	desc = NULL;   /* [한국어] 댕글링 방지 */
}



