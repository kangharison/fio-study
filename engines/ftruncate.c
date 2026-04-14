/*
 * [한국어 설명] ftruncate I/O 엔진 구현 (ftruncate.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 ftruncate(2) 시스템 호출을 I/O 동작으로 모사하는 fio I/O 엔진을 구현한다.
 * 실제로 디스크에 데이터를 읽거나 쓰지 않고, WRITE 방향 요청이 들어올 때마다
 * 파일의 논리적 크기를 io_u->offset으로 변경한다. 이를 통해 파일 시스템의
 * 메타데이터 갱신 경로(아이노드 크기 갱신, 블록 할당/해제 비용)를 벤치마크할 수 있다.
 * 본 파일은 fio의 I/O 엔진 플러그인 계약(ioengine_ops) 중 queue 콜백 하나만
 * 구현하는 가장 단순한 엔진 중 하나이다. 그 외 콜백은 fio가 제공하는
 * generic_* 공용 구현을 재사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 실행 흐름 main() → fio_backend() → thread_main() → do_io() 루프에서
 * 잡(thread_data) 하나가 I/O를 발행할 때 td_io_queue()가 호출되고, 그 내부에서
 * 현재 엔진에 등록된 ops->queue 함수 포인터를 호출한다. ftruncate 엔진을
 * 사용할 경우(--ioengine=ftruncate) 그 콜백이 본 파일의 fio_ftruncate_queue가 된다.
 * 실행 컨텍스트는 호스트 유저스페이스의 잡 스레드(또는 포크된 잡 프로세스)이며,
 * 커널 측 VFS 계층이 실제 truncate 작업을 수행한다.
 * 호출 체인:
 *   fio_backend() → thread_main() → do_io() → td_io_queue() → fio_ftruncate_queue()
 *   → ftruncate(2) → (VFS/파일시스템 truncate 경로)
 *
 * === 타 모듈과의 연결 ===
 * - 상위: ioengines.c(td_io_queue)로부터 호출되며, io_u.c(get_io_u/put_io_u)가
 *   준비한 io_u 구조체(offset/ddir 등)를 입력으로 받는다.
 * - 하위: POSIX의 ftruncate(2)만 직접 호출하고, sync 계열 방향(DDIR_SYNC 등)이
 *   들어오면 engines 공통 헬퍼 do_io_u_sync()에 위임한다.
 * - 파일 관리: open_file/close_file/get_file_size는 filesetup.c의
 *   generic_open_file/generic_close_file/generic_get_file_size를 그대로 사용해
 *   fio_file::fd 필드와 파일 크기 정보를 공유한다.
 * - 데이터 흐름: 실제 데이터 버퍼(io_u->xfer_buf)는 사용하지 않는다.
 *   입력은 io_u->offset(새 파일 크기)뿐이며, 완료 후 통계는
 *   FIO_Q_COMPLETED 반환을 통해 ioengines 코어가 동기 경로로 집계한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_ftruncate_queue(): ioengine_ops.queue 콜백. WRITE면 ftruncate 실행,
 *   sync 계열이면 do_io_u_sync로 위임, 그 외엔 EINVAL.
 * - fio_syncio_register(): 프로세스 로드 시(fio_init attribute) 엔진을
 *   ioengines 레지스트리에 등록.
 * - fio_syncio_unregister(): 프로세스 종료 시(fio_exit attribute) 등록 해제.
 * - ioengine: 본 엔진의 plugin vtable. name/version/queue와 generic_* 공용
 *   콜백, 그리고 동기 I/O 성격을 알리는 플래그 집합(FIO_SYNCIO/FAKEIO/SYNCFS)을
 *   담는다.
 */

/*
 * ftruncate: ioengine for https://git.kernel.org/pub/scm/linux/kernel/git/axboe/fio
 *
 * IO engine that does regular truncates to simulate data transfer
 * as fio ioengine.
 * DDIR_WRITE does ftruncate
 *
 */
#include <errno.h>      /* [한국어] errno 전역 변수와 EINVAL 상수를 얻기 위해 포함. ftruncate 실패 시 errno를 io_u->error로 전달한다. */
#include <unistd.h>     /* [한국어] POSIX ftruncate(2) 함수 프로토타입을 포함. 이 엔진의 유일한 시스템 호출이다. */

#include "../fio.h"     /* [한국어] fio 공용 타입(thread_data, io_u, fio_file, ioengine_ops)과 헬퍼(fio_ro_check, do_io_u_sync, register_ioengine, generic_* 콜백)를 한꺼번에 가져온다. */

/*
 * [한국어]
 * fio_ftruncate_queue - ftruncate 엔진의 I/O 제출(queue) 콜백
 *
 * @td:   현재 잡을 나타내는 thread_data. 읽기 전용 잡인지 검사(fio_ro_check)하거나
 *        sync 계열 방향 처리에 사용된다. do_io() 루프가 넘겨준다.
 * @io_u: 이번에 처리할 I/O 유닛. get_io_u()로 할당되어 prep 단계를 거친 뒤 여기에
 *        도달한다. 사용 필드는 io_u->file(대상 파일), io_u->ddir(방향),
 *        io_u->offset(새 파일 크기), io_u->error(에러 반환 슬롯).
 * @return: 항상 FIO_Q_COMPLETED를 반환한다. ftruncate는 동기 시스템 호출이므로
 *          queue 단계에서 즉시 완료되며, 에러는 io_u->error에 실어 전달한다.
 *          호출자(td_io_queue)는 이 반환값을 보고 별도 getevents 없이 통계 집계로
 *          진입한다.
 *
 * 이 함수가 필요한 이유: fio의 엔진 계약상 모든 엔진은 queue 콜백을 반드시
 * 제공해야 하며, 동기 엔진(FIO_SYNCIO)은 queue 단계 내에서 I/O를 완결짓는다.
 * 본 엔진은 WRITE 요청을 ftruncate 호출로 치환함으로써 파일 크기 변경의
 * 메타데이터 비용을 측정한다.
 *
 * 동작 단계:
 *  1) fio_ro_check: 이 잡이 읽기 전용으로 선언됐는데 WRITE를 시도하는지 검증.
 *  2) 방향 분기: DDIR_WRITE → ftruncate 실행, ddir_sync(fsync/fdatasync/sync_file_range)
 *     → do_io_u_sync 위임, 그 외(READ/TRIM) → EINVAL.
 *  3) ftruncate 실패 시 errno를 io_u->error에 기록.
 *  4) FIO_Q_COMPLETED 반환.
 *
 * 실행 컨텍스트: 잡 스레드(또는 잡 프로세스). 동일 io_u는 단일 스레드에서만
 * 다뤄지므로 별도 락은 필요하지 않다.
 *
 * 호출 체인:
 *   td_io_queue() → [fio_ftruncate_queue] → ftruncate(2) / do_io_u_sync()
 */
static enum fio_q_status fio_ftruncate_queue(struct thread_data *td,
					     struct io_u *io_u)
{
	struct fio_file *f = io_u->file;   /* [한국어] io_u가 가리키는 대상 파일 핸들을 지역 변수에 캐시. fio_file::fd(커널 파일 디스크립터)를 ftruncate에 넘기기 위함. */
	int ret = 0;                       /* [한국어] 시스템 호출 반환값 저장용. 0 = 성공, -1 = 실패(errno 설정). 초기값 0은 '에러 없음' 기본 상태. */

	fio_ro_check(td, io_u);            /* [한국어] 잡이 read-only 모드(td->o.read_only)인데 WRITE/TRIM을 시도하면 assert로 중단. 구성 실수를 조기에 잡는 방어 코드. */

	if (io_u->ddir == DDIR_WRITE)                 /* [한국어] WRITE 방향만 실제 ftruncate로 매핑. fio의 '쓰기' 의미를 '파일 크기 변경'으로 재해석한다. */
		ret = ftruncate(f->fd, io_u->offset); /* [한국어] ftruncate(2): 파일을 offset 바이트로 잘라/확장. 확장 시 구멍(hole)은 0으로 보인다. 커널은 VFS→FS의 setattr 경로로 아이노드 크기와 블록 맵을 갱신한다. */
	else if (ddir_sync(io_u->ddir))               /* [한국어] DDIR_SYNC/DDIR_DATASYNC/DDIR_SYNC_FILE_RANGE 여부 판정. sync성 요청은 엔진별로 특별 처리가 필요 없으므로 공통 헬퍼에 위임. */
		ret = do_io_u_sync(td, io_u);         /* [한국어] engines 공통 헬퍼: ddir 종류에 따라 fsync/fdatasync/sync_file_range를 내부적으로 호출하고 ret/errno를 그대로 돌려준다. */
	else
		io_u->error = EINVAL;                 /* [한국어] READ/TRIM 등 이 엔진이 지원하지 않는 방향 → 즉시 EINVAL로 에러 표시. ret은 0 유지해 아래 if(ret) 분기를 피하고, io_u->error만 남긴다. */

	if (ret)                                      /* [한국어] 시스템 호출이 실패(-1)했으면 진입. EINVAL 분기는 ret=0이므로 여기로 오지 않는다. */
		io_u->error = errno;                  /* [한국어] 글로벌 errno를 io_u에 저장해 상위(ioengines 통계)에서 에러 카운터·리포트에 반영되도록 한다. */

	return FIO_Q_COMPLETED;                       /* [한국어] 동기 엔진 관례: 완료됐음을 즉시 알린다. 호출자(td_io_queue)는 이 값을 보고 getevents를 건너뛰고 바로 completion 처리로 진입한다. */
}

/*
 * [한국어]
 * ioengine - 본 파일이 export하는 I/O 엔진 플러그인 디스크립터
 *
 * fio는 load_ioengine 시 register_ioengine으로 등록된 이 구조체의 콜백
 * 포인터를 td->io_ops에 연결하고, 이후 td_io_* 래퍼가 이 포인터를 호출한다.
 */
static struct ioengine_ops ioengine = {
	.name		= "ftruncate",
	/* [한국어] 엔진 이름. --ioengine=ftruncate 로 선택할 때 사용되는 키.
	 * 설정자: 정적 초기화(본 라인). 읽는 자: ioengines.c의 이름 매칭 로직.
	 * 값 범위: 프로세스 수명 동안 불변인 NUL 종료 문자열 리터럴. */

	.version	= FIO_IOOPS_VERSION,
	/* [한국어] 엔진 ABI 버전. fio 코어와 엔진의 struct 레이아웃 호환성 검사용.
	 * 설정자: 컴파일 시 fio.h의 매크로 값으로 고정. 읽는 자: register_ioengine.
	 * 값 범위: fio 릴리스마다 증가하는 정수. 불일치 시 등록 거부. */

	.queue		= fio_ftruncate_queue,
	/* [한국어] I/O 제출 콜백. td_io_queue에서 매 io_u마다 호출된다.
	 * 설정자: 정적 초기화. 읽는 자: ioengines.c.
	 * 값 범위: 본 파일 내 정의된 유일한 구현. NULL이면 엔진이 동작 불가. */

	.open_file	= generic_open_file,
	/* [한국어] 파일 열기 콜백을 filesetup.c의 공용 구현으로 재사용.
	 * 설정자: 정적 초기화. 읽는 자: td_io_open_file.
	 * 동작: open(2)로 fio_file::fd를 설정. ftruncate에는 별도 처리가 불필요하여 공용본을 그대로 사용. */

	.close_file	= generic_close_file,
	/* [한국어] 파일 닫기 콜백도 공용 구현 사용 — close(2)로 fd 해제. */

	.get_file_size	= generic_get_file_size,
	/* [한국어] 파일 크기 질의 콜백도 공용 구현 사용 — fstat(2)로 real_file_size 설정. */

	.flags		= FIO_SYNCIO | FIO_FAKEIO | FIO_SYNCFS,
	/* [한국어] 엔진 속성 비트마스크.
	 *   FIO_SYNCIO : 동기 엔진 — queue 단계에서 I/O가 즉시 완료된다(별도 getevents 불필요).
	 *   FIO_FAKEIO : 실제 데이터 전송이 없는 '페이크' I/O — fio가 버퍼/rwmix 검증 등
	 *                데이터 기반 처리를 건너뛰도록 알린다.
	 *   FIO_SYNCFS : 파일시스템 sync 방향을 처리할 수 있음을 표시(ddir_sync 분기와 짝).
	 * 읽는 자: ioengines.c/backend.c의 플래그 기반 분기들. */
};

/*
 * [한국어]
 * fio_syncio_register - 엔진 생성자. 공유 오브젝트 로드 시 자동 실행되어
 * 전역 ioengine 레지스트리에 본 엔진을 등록한다.
 *
 * @return: void. 등록 실패는 내부에서 log_err로 보고되며 치명적이지 않다.
 *
 * 실행 컨텍스트: 메인 프로세스의 ELF constructor 단계(main() 진입 이전) 또는
 * 동적 로딩 시점. fio_init는 compiler/compiler.h에서 constructor attribute로
 * 정의되어 있다.
 *
 * 호출 체인: (dynamic loader / ELF ctor) → [fio_syncio_register] → register_ioengine()
 */
static void fio_init fio_syncio_register(void)
{
	register_ioengine(&ioengine);  /* [한국어] ioengines.c가 관리하는 전역 링크드 리스트에 본 플러그인을 append. 이후 --ioengine=ftruncate로 이름 매칭이 가능해진다. */
}

/*
 * [한국어]
 * fio_syncio_unregister - 엔진 소멸자. 프로세스 종료 시 레지스트리에서 제거한다.
 *
 * @return: void.
 *
 * 실행 컨텍스트: ELF destructor 단계(atexit 유사). fio_exit 매크로가
 * destructor attribute를 제공한다.
 *
 * 호출 체인: (atexit / ELF dtor) → [fio_syncio_unregister] → unregister_ioengine()
 */
static void fio_exit fio_syncio_unregister(void)
{
	unregister_ioengine(&ioengine);  /* [한국어] 등록 리스트에서 본 엔진 노드를 제거해 dangling reference를 방지. 동적 로드(shared plugin) 시나리오에서 중요하다. */
}
