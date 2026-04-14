/*
 * glusterfs engine
 *
 * IO engine using Glusterfs's gfapi sync interface
 *
 */

/*
 * [한국어 설명] GlusterFS 동기 gfapi I/O 엔진 (glusterfs_sync.c)
 *
 * === 파일의 역할 ===
 * GlusterFS의 유저스페이스 동기 I/O 인터페이스(libgfapi의 glfs_read/glfs_write)를
 * 이용하는 fio I/O 엔진 "gfapi"를 구현한다. 요청이 들어오면 즉시 blocking 호출로
 * 완료하는 단순 엔진이며, FIO_SYNCIO 플래그로 fio 코어가 이 엔진을 동기 엔진으로
 * 취급하도록 한다. FIO_DISKLESSIO 플래그로 로컬 fd 기반 파일 크기 탐색을 건너뛴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio --ioengine=gfapi 옵션으로 선택되는 플러그인으로, ioengines.c가 관리하는
 * ioengine_ops 레지스트리에 fio_gf_register()(fio_init 생성자)로 등록된다. 실행 흐름은
 * backend.c → td_io_init → fio_gf_setup → (open_file) → 각 io_u에 대해 td_io_prep →
 * td_io_queue → fio_gf_queue → glfs_read/write 순이며, 엔진 자체는 commit/getevents를
 * 구현하지 않고 FIO_Q_COMPLETED를 즉시 반환한다. 단일 잡 스레드(유저스페이스)에서 실행.
 *
 * === 타 모듈과의 연결 ===
 * 상단: fio 코어(ioengines.c의 td_io_queue/td_io_prep)에서 호출된다.
 * 하단: glusterfs.c의 공통 헬퍼(fio_gf_setup/cleanup/open_file/close_file/unlink_file/
 *       get_file_size)와 libgfapi의 glfs_read/glfs_write/glfs_fsync/glfs_fdatasync/
 *       glfs_lseek를 호출한다.
 * 데이터 흐름: io_u->xfer_buf ↔ glfs_read/write → GlusterFS translator stack → 원격 브릭.
 * LAST_POS(file)은 fio_file::engine_pos 필드를 빌려 엔진 전용 상태로 사용한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_gf_prep(): glfs_lseek으로 I/O 오프셋을 맞춤(중복 lseek 회피).
 * - fio_gf_queue(): ddir에 따라 glfs_read/glfs_write/glfs_fsync/glfs_fdatasync 호출.
 * - ioengine 변수: fio가 동적으로 로드하는 엔진 디스크립터(콜백 테이블).
 * - fio_gf_register()/fio_gf_unregister(): 프로세스 시작/종료 시 엔진 등록/해제.
 */

#include "gfapi.h"
/* [한국어] gfapi.h: GlusterFS libgfapi 래퍼 헤더. struct gf_data(엔진별 per-td 상태: glfs_t *fs, glfs_fd_t *fd),
 * struct gf_options, gfapi_options[] 및 fio_gf_setup/cleanup/open_file/close_file/unlink_file/get_file_size
 * 공통 헬퍼 선언을 포함한다. 동기/비동기(glusterfs_sync.c, glusterfs_async.c) 두 엔진이 공유. */

#define LAST_POS(f)	((f)->engine_pos)
/* [한국어] 매크로 LAST_POS: fio_file::engine_pos 필드를 "마지막 I/O 오프셋 + 길이" 캐시로 빌려 쓴다.
 * engine_pos는 fio 코어가 엔진 사용을 허용하는 엔진 전용 상태 슬롯이며, sync 계열 엔진들이 관례적으로
 * 중복 lseek 회피 용도로 사용한다. 초기값 -1ULL(전체 비트 1)은 "아직 값 없음" 센티넬. */

/*
 * [한국어]
 * fio_gf_prep - GlusterFS 동기 엔진의 I/O 준비 콜백 (ioengine_ops::prep)
 *
 * @td: 이 잡을 소유하는 thread_data. td->io_ops_data에 fio_gf_setup()이 할당한 struct gf_data*가 들어있다.
 * @io_u: 이번에 준비할 I/O 유닛. io_u->file, io_u->offset, io_u->ddir가 이미 fio 코어에 의해 세팅됨.
 * @return: 0=성공(queue 단계로 진행), 0이 아니면 실패(td_verror로 에러 등록되어 잡 중단).
 *
 * 동기/배경: GlusterFS의 동기 gfapi는 read/write가 명시 offset을 받지 않고 fd의 현재 위치에서 수행되므로,
 * queue 호출 직전에 glfs_lseek으로 오프셋을 맞춰야 한다. 직전 I/O 종료 위치와 동일하면 lseek을 생략해
 * 시스템 호출 비용(유저→libgfapi→translator)을 절약한다.
 * 실행 컨텍스트: 잡 스레드(유저스페이스, 단일 스레드)에서 io_u 하나당 1회 호출. 재진입 없음.
 * caller: ioengines.c::td_io_prep() — get_io_u() 뒤 td_io_queue() 앞 단계.
 * callee: libgfapi의 glfs_lseek(POSIX lseek와 같은 의미로 gfapi fd 위치 이동).
 * 에러 경로: glfs_lseek<0 → td_verror()로 errno 기록 → 호출자에서 잡 실패 처리.
 *
 * 호출 체인: backend.c thread_main → td_io_prep → [fio_gf_prep] → glfs_lseek
 */
static int fio_gf_prep(struct thread_data *td, struct io_u *io_u)
{
	struct fio_file *f = io_u->file;
	/* [한국어] 이번 I/O 대상 파일 기술자. io_u->file은 get_io_u()가 라운드로빈/랜덤 선택한 fio_file.
	 * engine_pos(LAST_POS) 캐시는 fio_file별로 독립적이므로 여기서 꺼내온다. */
	struct gf_data *g = td->io_ops_data;
	/* [한국어] per-td 엔진 상태. g->fd(glfs_fd_t*)가 이미 open_file 단계(fio_gf_open_file)에서 열려 있다.
	 * 주의: fio_file f와 별개로 GlusterFS 핸들은 g->fd 하나에만 저장되는 단순 엔진 모델. */

	dprint(FD_FILE, "fio prep\n");
	/* [한국어] FD_FILE 디버그 채널로 prep 진입 로깅 (fio --debug=file 옵션으로 활성화). */

	if (!ddir_rw(io_u->ddir))
		/* [한국어] DDIR_READ/DDIR_WRITE가 아니면(= fsync/datasync/trim 등) 오프셋 조정 불필요 → 즉시 성공. */
		return 0;

	if (LAST_POS(f) != -1ULL && LAST_POS(f) == io_u->offset)
		/* [한국어] 센티넬(-1ULL)이 아니고 직전 I/O가 끝난 위치와 이번 요청 시작이 일치 → 순차 I/O이므로
		 * 커널 공간까지 내려가는 lseek syscall 생략. sequential 워크로드에서 syscall 수를 대략 절반으로 줄임. */
		return 0;

	if (glfs_lseek(g->fd, io_u->offset, SEEK_SET) < 0) {
		/* [한국어] 절대 오프셋으로 gfapi fd 위치 이동. SEEK_SET: 파일 시작 기준 절대 위치.
		 * 실패 시 errno 설정됨(EINVAL: 음수 offset, EBADF: fd 무효 등). */
		td_verror(td, errno, "lseek");
		/* [한국어] fio 코어에 에러 기록: td->error, td->verror 문자열 세팅 → 잡은 이후 중단됨. */
		return 1;
		/* [한국어] 0이 아닌 값 반환 = prep 실패. td_io_queue는 호출되지 않는다. */
	}

	return 0;
	/* [한국어] 정상 경로: queue 단계에서 동기 read/write가 올바른 오프셋에서 시작됨. */
}

/*
 * [한국어]
 * fio_gf_queue - GlusterFS 동기 엔진의 I/O 제출 콜백 (ioengine_ops::queue)
 *
 * @td: 잡 컨텍스트. td->io_ops_data = struct gf_data* (glfs_fd_t *fd 포함).
 * @io_u: 제출할 I/O 유닛. ddir(방향), xfer_buf(데이터 버퍼), xfer_buflen(바이트 수)가 세팅됨.
 *        ddir에 따라 DDIR_READ/WRITE는 실제 데이터 전송, DDIR_SYNC/DATASYNC는 메타/데이터 flush.
 * @return: fio_q_status 열거값. 동기 엔진이므로 항상 FIO_Q_COMPLETED(="이미 완료됨") 반환 →
 *          fio 코어는 commit/getevents를 거치지 않고 곧바로 io_u 완료 처리.
 *
 * 동기/배경: FIO_SYNCIO 플래그가 설정된 엔진은 queue 호출 내에서 I/O를 완전히 끝낸다.
 * 성공/부분성공/실패 모두 io_u의 필드(error/resid)에 결과를 담아 FIO_Q_COMPLETED로 반환한다.
 * 실행 컨텍스트: 잡 스레드(유저스페이스). glfs_read/write 내부에서 GlusterFS translator 스택 및
 * 네트워크 I/O로 블로킹됨 → 잡 스레드는 완료까지 슬립 상태.
 * caller: ioengines.c::td_io_queue() — 내부에서 io_u_submit()이 이 콜백을 invoke.
 * callee: libgfapi glfs_read/glfs_write/glfs_fsync/glfs_fdatasync.
 * 에러 경로: ret<0 → errno를 io_u->error에 복사 → td_verror로 잡 실패 등록 → 코어가 통계에 실패 반영.
 * 부분 완료: 0 ≤ ret < xfer_buflen → io_u->resid에 남은 바이트 기록 → 코어가 재시도 또는 단축 완료 처리.
 *
 * 호출 체인: td_io_queue → [fio_gf_queue] → glfs_read/glfs_write/glfs_fsync/glfs_fdatasync
 */
static enum fio_q_status fio_gf_queue(struct thread_data *td, struct io_u *io_u)
{
	struct gf_data *g = td->io_ops_data;
	/* [한국어] per-td GlusterFS 상태. g->fd는 open_file()에서 glfs_open/glfs_creat로 획득된 gfapi 핸들. */
	int ret = 0;
	/* [한국어] 각 gfapi 호출의 반환값(전송된 바이트 수 또는 음수 에러) 저장용. */

	dprint(FD_FILE, "fio queue len %llu\n", io_u->xfer_buflen);
	/* [한국어] FD_FILE 채널로 전송 길이 로깅 — fio --debug=file 시 출력. */
	fio_ro_check(td, io_u);
	/* [한국어] 읽기 전용(readonly) 잡 옵션이 걸려 있는데 DDIR_WRITE/TRIM이면 즉시 abort.
	 * td->o.read_only 모드에서 실수로 write가 들어오는 것을 방지하는 가드. */

	if (io_u->ddir == DDIR_READ)
		/* [한국어] 읽기: gfapi fd에서 현재 오프셋(prep에서 lseek됨)부터 xfer_buflen만큼 읽는다.
		 * 마지막 인자 0은 flags(현재는 의미 없음). 반환: 읽은 바이트, 0=EOF, 음수=에러. */
		ret = glfs_read(g->fd, io_u->xfer_buf, io_u->xfer_buflen, 0);
	else if (io_u->ddir == DDIR_WRITE)
		/* [한국어] 쓰기: GlusterFS 서버의 해당 파일 오프셋으로 xfer_buflen 바이트 전송.
		 * 동기 인터페이스이지만 실제 디스크 영속성은 보장되지 않음(뒤의 fsync/datasync가 필요). */
		ret = glfs_write(g->fd, io_u->xfer_buf, io_u->xfer_buflen, 0);
	else if (io_u->ddir == DDIR_SYNC)
/* [한국어] CONFIG_GF_NEW_API: configure 스크립트가 libgfapi의 새 fsync ABI(콜백+데이터 인자 포함,
 * GlusterFS 6.0+ 경향) 존재 여부를 감지해 정의. 구버전은 fd만 받는 단일 인자 시그니처. */
#if defined(CONFIG_GF_NEW_API)
		/* [한국어] 신 API: (fd, post_callback, cookie). 동기 사용 시 콜백 NULL로 두면 blocking. */
		ret = glfs_fsync(g->fd, NULL, NULL);
#else
		/* [한국어] 구 API: 단순 blocking fsync. 메타데이터+데이터 영속화 요청. */
		ret = glfs_fsync(g->fd);
#endif
	else if (io_u->ddir == DDIR_DATASYNC)
#if defined(CONFIG_GF_NEW_API)
		/* [한국어] 신 API datasync: 데이터만 영속화(메타데이터는 강제하지 않음) — POSIX fdatasync와 동치. */
		ret = glfs_fdatasync(g->fd, NULL, NULL);
#else
		/* [한국어] 구 API datasync. */
		ret = glfs_fdatasync(g->fd);
#endif
	else {
		/* [한국어] DDIR_TRIM 등 gfapi가 지원하지 않는 연산이 들어온 경우. */
		log_err("unsupported operation.\n");
		io_u->error = EINVAL;
		/* [한국어] io_u->error에 EINVAL 표시 → 코어가 실패 통계에 계상. */
		return FIO_Q_COMPLETED;
		/* [한국어] 동기 엔진 계약상 상태값은 COMPLETED로 반환하고 에러는 io_u->error로 전달. */
	}
	dprint(FD_FILE, "fio len %llu ret %d\n", io_u->xfer_buflen, ret);
	/* [한국어] 전송 요청 길이와 실제 반환값 로깅 — 짧은 read/write 디버깅에 유용. */
	if (io_u->file && ret >= 0 && ddir_rw(io_u->ddir))
		/* [한국어] 성공적 read/write 후에만 LAST_POS 갱신. fsync/에러 경로에서는 오프셋 무의미하므로 스킵.
		 * 다음 io_u의 prep에서 이 값과 io_u->offset 비교해 중복 lseek을 피함. */
		LAST_POS(io_u->file) = io_u->offset + ret;

	if (ret != (int)io_u->xfer_buflen) {
		/* [한국어] 요청량과 실제 전송량이 다른 경우(부분 완료 또는 에러). */
		if (ret >= 0) {
			/* [한국어] 부분 완료(short read/write): EOF 근접 또는 쓰기 제약. */
			io_u->resid = io_u->xfer_buflen - ret;
			/* [한국어] 잔여 바이트를 resid에 기록 → fio 코어가 남은 부분 재큐잉 또는 통계 반영. */
			io_u->error = 0;
			/* [한국어] 에러 아님 명시. 부분 완료는 정상 경로. */
			return FIO_Q_COMPLETED;
		} else
			/* [한국어] ret<0: 실패. errno(libgfapi가 설정)를 io_u->error에 저장. */
			io_u->error = errno;
	}

	if (io_u->error) {
		/* [한국어] 실제 에러가 등록된 경우에만 로그 + td 에러 기록. */
		log_err("IO failed.\n");
		td_verror(td, io_u->error, "xfer");
		/* [한국어] td->error/td->verror에 "xfer" 문맥과 함께 기록 → 잡 종료 시 리포트에 포함. */
	}

	return FIO_Q_COMPLETED;
	/* [한국어] 동기 엔진이므로 어떤 경로로 왔든 "이미 완료됨"으로 반환. fio 코어는 commit/getevents를
	 * 호출하지 않고 바로 io_u 완료 루틴(put_io_u, 통계 집계)으로 진입. */

}

/*
 * [한국어]
 * struct ioengine_ops ioengine — fio I/O 엔진 플러그인 디스크립터 (정적 전역).
 * fio_init 생성자(fio_gf_register)에서 ioengines.c의 전역 리스트에 등록되며,
 * --ioengine=gfapi로 잡에서 선택되면 ioengine_ops::clone을 통해 td 별 복사본이 만들어진다.
 * FIO_SYNCIO + FIO_DISKLESSIO 플래그 조합은 "동기 API + 로컬 fd 기반 크기 조회 스킵"을 의미한다.
 */
static struct ioengine_ops ioengine = {
	.name = "gfapi",
	/* [한국어] 엔진 이름. --ioengine=gfapi 또는 ioengine=gfapi(잡 파일)로 선택.
	 * 읽는 자: ioengines.c의 load_ioengine()이 문자열 매칭. */
	.version = FIO_IOOPS_VERSION,
	/* [한국어] fio가 기대하는 ioengine ABI 버전. 불일치 시 load_ioengine 거부 → 엔진 버전 sanity 체크. */
	.init = fio_gf_setup,
	/* [한국어] 잡 초기화 콜백(glusterfs.c 공통): glfs_new/glfs_set_volfile_server/glfs_init로
	 * volume 마운트. td->io_ops_data에 gf_data 할당. td_io_init에서 1회 호출. */
	.cleanup = fio_gf_cleanup,
	/* [한국어] 잡 종료 정리(glusterfs.c 공통): glfs_fini로 volume 해제, gf_data 해제. */
	.prep = fio_gf_prep,
	/* [한국어] 위에서 정의한 오프셋 세팅 콜백. 매 io_u마다 td_io_prep에서 호출. */
	.queue = fio_gf_queue,
	/* [한국어] 위에서 정의한 동기 I/O 제출 콜백. 동기 완료를 보장. */
	.open_file = fio_gf_open_file,
	/* [한국어] 잡 시작 시 각 fio_file에 대해 glfs_open/glfs_creat 수행 (glusterfs.c 공통). */
	.close_file = fio_gf_close_file,
	/* [한국어] 잡 종료/파일 스위치 시 glfs_close 수행 (공통). */
	.unlink_file = fio_gf_unlink_file,
	/* [한국어] unlink=1 옵션 시 glfs_unlink로 파일 삭제 (공통). */
	.get_file_size = fio_gf_get_file_size,
	/* [한국어] filesize 자동 결정용: glfs_stat로 파일 크기 조회. FIO_DISKLESSIO 플래그로
	 * 로컬 fd stat을 건너뛰므로 이 콜백이 필수 (공통 헬퍼). */
	.options = gfapi_options,
	/* [한국어] gfapi 전용 옵션 테이블(volume, brick_host 등). fio 옵션 파서가 파싱. */
	.option_struct_size = sizeof(struct gf_options),
	/* [한국어] 옵션을 저장할 per-td 구조체 크기 — fio 코어가 이 크기로 zalloc. */
	.flags = FIO_SYNCIO | FIO_DISKLESSIO,
	/* [한국어] FIO_SYNCIO: queue 내부에서 동기 완료(commit/getevents 불필요, 지연 측정도 단순화).
	 * FIO_DISKLESSIO: 실제 OS 파일 디스크립터가 없음 → fio 코어의 fd 기반 stat/ftruncate 등 우회. */
};

/*
 * [한국어]
 * fio_gf_register - 프로세스 시작 시 엔진을 전역 레지스트리에 등록.
 * 속성 fio_init(= __attribute__((constructor)))으로 main() 진입 이전에 자동 호출.
 * 실행 컨텍스트: 프로세스 단일 스레드(초기화). caller: dynamic linker(ELF .init_array).
 * callee: ioengines.c::register_ioengine — 전역 ioengine_list에 삽입.
 */
static void fio_init fio_gf_register(void)
{
	register_ioengine(&ioengine);
	/* [한국어] 위 ioengine 디스크립터를 fio 전역 리스트에 추가 → load_ioengine("gfapi")로 참조 가능해짐. */
}

/*
 * [한국어]
 * fio_gf_unregister - 프로세스 종료 시 엔진 레지스트리에서 제거.
 * 속성 fio_exit(= __attribute__((destructor)))로 exit() 시점 자동 호출.
 * 실행 컨텍스트: 프로세스 종료. caller: libc atexit 경로. callee: unregister_ioengine.
 */
static void fio_exit fio_gf_unregister(void)
{
	unregister_ioengine(&ioengine);
	/* [한국어] 전역 리스트에서 엔진 노드 제거 — 동적 로드/언로드 환경(공유 라이브러리) 정합성 유지. */
}
