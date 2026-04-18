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

/*
 * [한국어] 엔진 전용 옵션 테이블.
 *
 * 공통 규약 (fio 옵션 테이블):
 *  - `.name`:   CLI/잡 파일에서 사용자가 입력하는 키워드.
 *  - `.lname`:  `--enghelp` 출력 시 보이는 사람-읽기 라벨.
 *  - `.type`:   파서 선택(FIO_OPT_BOOL/INT/STR_STORE 등). 값 해석 방법 결정.
 *  - `.off1`:   `struct fio_mtd_options` 내 저장 위치의 offsetof.
 *  - `.help`:   짧은 한 줄 설명(--cmdhelp=옵션명 에 표시).
 *  - `.hide`:   1이면 `--cmdhelp` 전체 목록에서 감춤(엔진 세부 옵션용).
 *  - `.def`:    사용자가 지정하지 않았을 때의 기본 문자열 값.
 *  - `.category/.group`: 옵션 그룹화(도움말 섹션 분류).
 *  - NULL 엔트리(`{ .name = NULL }`)로 배열 종단을 표시(옵션 파서가 순회 종료).
 */
static struct fio_option options[] = {
	{
		.name	= "skip_bad",
		/* [한국어] 사용자 CLI/잡파일에서 `--skip_bad=1` / `skip_bad=1` 로 지정. */

		.lname	= "Skip operations against bad blocks",
		/* [한국어] `fio --enghelp=mtd` 출력에서 사람이 읽는 긴 라벨. */

		.type	= FIO_OPT_BOOL,
		/* [한국어] 0/1 부울 파서 사용 — "0"/"1"/"true"/"false" 등 문자열 모두 해석. */

		.off1	= offsetof(struct fio_mtd_options, skip_bad),
		/* [한국어] 파서가 구조체 내 이 오프셋에 값을 기록 — `td->eo + off1` 로 접근. */

		.help	= "Skip operations against known bad blocks.",
		/* [한국어] `--cmdhelp=skip_bad` 출력 본문 — bad block 을 건너뛸지 여부. */

		.hide	= 1,
		/* [한국어] 1이므로 일반 `--cmdhelp` 전체 목록에는 감추고,
		 * MTD 엔진 문맥에서만 노출(엔진 세부 옵션이라는 표시). */

		.def	= "0",
		/* [한국어] 미지정 시 기본값 — 0(bad block 스킵 안 함, 그대로 접근해 에러 드러냄). */

		.category = FIO_OPT_C_ENGINE,
		/* [한국어] 대분류 — "엔진 관련 옵션" (다른 대분류로 FIO_OPT_C_GENERAL, _IO 등). */

		.group	= FIO_OPT_G_MTD,
		/* [한국어] 세부 그룹 — MTD 엔진 전용. optgroup.h 에서 정의. */
	},
	{
		.name	= NULL,
		/* [한국어] 배열 종단자 — 옵션 파서가 .name==NULL 만나면 순회 종료. */
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

/*
 * [한국어] MTD 엔진의 공개 콜백 테이블 — fio 코어의 ioengine_ops 계약 구현체.
 * 각 필드는 다음 규약을 따른다:
 *  - 설정된 콜백만 fio 코어가 호출하며, 미설정 필드(예: .init/.cleanup/.commit/.getevents/
 *    .event/.prep/.invalidate/.unlink_file/.io_u_init/.io_u_free/.errdetails/.iomem_alloc/
 *    .iomem_free/.cancel/.get_max_open_zones/.report_zones/.reset_wp)는 "필요 없음" 을 의미.
 *  - 동기 엔진(FIO_SYNCIO) 이므로 .commit/.getevents/.event 는 불필요(queue가 곧 완료).
 *  - MTD 디바이스 자체가 파일 크기 고정(erase block 기반) 이라 확장 불가 → FIO_NOEXTEND.
 */
static struct ioengine_ops ioengine = {
	.name		= "mtd",
	/* [한국어] 엔진 식별 문자열 — 잡파일/CLI의 `ioengine=mtd`.
	 * 설정자: 고정 리터럴. 읽는 자: load_ioengine("mtd") 가 engine_list 선형 탐색. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] ioengine ABI 버전 — ioengines.c 의 check_engine_ops 가 일치 확인.
	 * fio 헤더와 컴파일된 .o 의 ABI 불일치 시 register_ioengine 에서 거부. */

	.queue		= fio_mtd_queue,
	/* [한국어] 메인 I/O 제출 콜백. FIO_SYNCIO 엔진이라 호출 내에서 read/write/erase 완료까지 수행.
	 * 반환 규약: FIO_Q_COMPLETED(동기), BUSY(거의 안 씀), QUEUED(본 엔진은 사용 안 함).
	 * 호출자: td_io_queue() → 본 콜백. */

	.open_file	= fio_mtd_open_file,
	/* [한국어] 파일당 1회 호출 — generic_open_file + mtd_get_dev_info + fmd 부착.
	 * 반환: 0 성공, !=0 실패. fio 코어가 잡 초기화/파일 재오픈 시 호출. */

	.close_file	= fio_mtd_close_file,
	/* [한국어] open_file 역순 — fmd 해제 + generic_close_file. */

	.get_file_size	= fio_mtd_get_file_size,
	/* [한국어] mtd_get_dev_info 결과로 f->real_file_size 기록.
	 * open_file 보다 먼저 호출되어 fio 코어의 I/O 범위/블록 인덱싱에 사용. */

	.flags		= FIO_SYNCIO | FIO_NOEXTEND,
	/* [한국어] 엔진 특성 비트 조합.
	 *  - FIO_SYNCIO:   queue() 가 호출 내에서 완료까지 수행(비동기 제출 불가).
	 *                  fio 코어의 io_u_sync_complete/fio_fill_issue_time 경로가 선택됨.
	 *  - FIO_NOEXTEND: 대상 파일 크기 확장 금지(MTD 파티션은 고정 크기) — fio 가
	 *                  요청 offset+len 이 size 를 넘으면 잘라냄/에러 처리.
	 * 미설정 비트 의미:
	 *  - FIO_DISKLESSIO:     파일시스템 파일 없이도 동작(MTD는 /dev/mtdN 필요 → unset).
	 *  - FIO_RAWIO:          직접 I/O(O_DIRECT 계열) — MTD는 libmtd 경로라 불필요.
	 *  - FIO_NOIO:           I/O 실제 수행 안 함(cpu 엔진용).
	 *  - FIO_PIPEIO:         파이프 기반(splice 엔진).
	 *  - FIO_BARRIER:        barrier 지원.
	 *  - FIO_UNIDIR:         단방향 I/O만 허용.
	 *  - FIO_NODISKUTIL:     diskutil 통계 비활성.
	 *  - FIO_MEMALIGN:       엔진 특정 메모리 정렬 요구.
	 *  - FIO_ASYNCIO_SYNC_TRIM: TRIM은 동기로 폴백.
	 *  - FIO_ASYNCIO_SETS_ISSUE_TIME: 엔진이 직접 issue_time 설정.
	 *  - FIO_RO:             읽기 전용 오픈 필요.
	 *  - FIO_MULTI_RANGE_TRIM: 다중 범위 TRIM.
	 *  - FIO_NO_OFFLOAD:     offload submit 금지.
	 *  (미설정 = fio 기본 동작 유지) */

	.options	= options,
	/* [한국어] 엔진 옵션 테이블(skip_bad) 포인터. */

	.option_struct_size	= sizeof(struct fio_mtd_options),
	/* [한국어] td->eo 메모리 할당 크기 — 엔진별 옵션 파서가 이 크기만큼 zalloc. */
};

/*
 * [한국어]
 * fio_mtd_register - MTD 엔진의 프로세스-로더 시점 생성자.
 *
 * `fio_init` = `__attribute__((constructor))` 로, ELF .init_array 에 들어가
 * ld.so 가 실행 파일/공유 라이브러리 로드 직후 main() 보다 먼저 호출한다.
 * 동적 엔진(.so 로 로드)이라면 dlopen 시점에 호출된다.
 *
 * 수행 내용:
 *  1) libmtd_open() 으로 프로세스 전역 libmtd 핸들 확보.
 *  2) register_ioengine(&ioengine) 로 fio 의 전역 engine_list 에 추가
 *     (내부적으로 flist_add_tail). 이후 load_ioengine("mtd") 가 이를 찾는다.
 *
 * 호출 체인: ld.so (.init_array) → [이 함수] → libmtd_open + register_ioengine.
 */
static void fio_init fio_mtd_register(void)
{
	desc = libmtd_open();           /* [한국어] libmtd 내부 상태 핸들 개방 — mtd_get_dev_info 등 모든 libmtd API 가 이 핸들을 사용. */
	register_ioengine(&ioengine);   /* [한국어] fio 전역 engine_list tail 에 &ioengine 을 flist_add — load_ioengine("mtd") 탐색 대상. */
}

/*
 * [한국어]
 * fio_mtd_unregister - 프로세스 종료 시점 소멸자.
 *
 * `fio_exit` = `__attribute__((destructor))` 로 .fini_array 에 들어가
 * atexit 체인에서 호출된다. 동적 엔진이라면 dlclose 시점.
 *
 * 수행 내용:
 *  1) unregister_ioengine 으로 engine_list 에서 제거(flist_del_init).
 *     이는 동일 .so 재로딩 시의 중복 등록을 방지.
 *  2) libmtd_close 로 전역 핸들 정리.
 *  3) desc = NULL 로 댕글링 방지(재로딩 시 stale 포인터 접근 방지).
 *
 * 호출 체인: atexit / ld.so (.fini_array) → [이 함수].
 */
static void fio_exit fio_mtd_unregister(void)
{
	unregister_ioengine(&ioengine);   /* [한국어] engine_list 에서 &ioengine 제거. 잔류 포인터 방지 위한 flist_del_init 수행. */
	libmtd_close(desc);               /* [한국어] libmtd 내부 자원(프로브 캐시 등) 해제. */
	desc = NULL;   /* [한국어] 댕글링 방지 — 재 .init_array 호출 시 NULL 에서 다시 할당 가능하게 함. */
}



