/*
 * [한국어 설명] GlusterFS API(gfapi) 공통 헤더 (gfapi.h)
 *
 * === 파일의 역할 ===
 * GlusterFS 분산 파일 시스템을 대상으로 하는 세 fio 엔진 ──
 * `glusterfs` (공통 진입점), `glusterfs_sync`(FIO_SYNCIO 기반 동기 경로),
 * `glusterfs_async`(비동기 큐/이벤트 기반) ── 이 공유하는 선언 모음이다.
 * 각 엔진은 자신의 `ioengine_ops`를 정의하되, 볼륨 마운트/파일 오픈/종료/통계
 * 수집 같은 공통 로직은 본 헤더의 `fio_gf_*` 함수를 재사용한다. 즉 이 파일은
 * "libgfapi의 glfs_t/glfs_fd_t 핸들 관리 + 공통 옵션 구조" 의 계약서이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio_backend 잡 루프는 load_ioengine("gfapi[_sync|_async]") 이후 setup→init→
 * get_file_size→open_file→queue/event/getevents→close_file→cleanup 순서로 엔진
 * 콜백을 호출한다. 본 헤더에서 선언된 `fio_gf_setup/cleanup/open_file/close_file/
 * unlink_file/get_file_size`는 `engines/glusterfs.c`에 정의되며, sync/async 변종은
 * queue/getevents만 직접 구현하고 나머지는 이 공통 함수를 엔진 ops에 그대로 꽂는다.
 * 실행 컨텍스트는 잡 스레드(`thread_data` 1개당 1개)이며, libgfapi 내부 IO 스레드풀과
 * 네트워크 TCP 커넥션은 libgfapi가 관리한다.
 *
 * === 타 모듈과의 연결 ===
 * - `<glusterfs/api/glfs.h>`: GlusterFS 유저스페이스 API — glfs_new/set_volfile_server/
 *   init/fini, glfs_open/close/pread/pwrite/preadv/pwritev, glfs_stat, 비동기 변종
 *   glfs_pread_async/pwrite_async + glfs_io_cbk 콜백 타입 등. `volume/brick/server`
 *   셋을 바탕으로 MON(관리자 노드)에 접속해 클러스터 토폴로지를 당겨오고 파일 I/O를 경유한다.
 * - `../fio.h`: thread_data, io_u, fio_file, ioengine_ops, td_verror 등 fio 코어 타입.
 * - `../optgroup.h` (간접): FIO_OPT_G_GFAPI 그룹이 `glusterfs.c`의 `gfapi_options[]`에서
 *   사용된다.
 * - 공유 상태: `gf_options`는 각 잡의 td->eo가 가리키며 잡 시작 후 불변. `gf_data`는
 *   FILE_ENG_DATA(f) 슬롯에 부착되어 잡 스레드 1명이 단독 접근한다. 단, single_instance=1
 *   일 때는 동일 볼륨 이름을 key로 glfs_t 핸들을 프로세스 공유(참조 카운트)로 재사용한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct gf_options: 잡 파일/CLI가 채우는 사용자 옵션 3종(volume/brick/single_instance).
 * - struct gf_data: 엔진 내부 실행 상태(glfs_t 마운트 핸들/glfs_fd_t 파일 핸들/비동기
 *   완료 io_u 링).
 * - fio_gf_setup(): 잡당 1회, glfs_new→set_volfile_server→init 으로 볼륨 마운트.
 * - fio_gf_cleanup(): fio_gf_setup의 역순 — glfs_fini 로 MON 연결 해제.
 * - fio_gf_open_file/close_file: 잡 파일 1개당 glfs_open/glfs_close 호출.
 * - fio_gf_unlink_file: 잡 종료 시 생성된 파일 제거 — filename이 create_on_open 이면 호출됨.
 * - fio_gf_get_file_size: glfs_stat 결과로 f->real_file_size 를 채운다(코어 I/O 범위 결정 근거).
 *
 * === 사용하는 라이브러리 ===
 * libgfapi (gluster-server/gluster-client 통신의 유저스페이스 초 — FUSE 우회).
 * 서버 측 glusterfs/glusterfsd 데몬에 TCP(기본 24007/24008)으로 접속한다.
 */

#include <glusterfs/api/glfs.h>    /* [한국어] glfs_t/glfs_fd_t/glfs_io_cbk/glfs_pread[v]/pwrite[v]_async 등 libgfapi 공개 API.
                                    *  이 헤더가 없으면 GlusterFS 볼륨/파일 핸들에 대한 접근 자체가 불가능. */
#include "../fio.h"                /* [한국어] thread_data, io_u, ioengine_ops, fio_file, td_verror, FILE_ENG_DATA 매크로 등
                                    *  fio 코어 전역 타입. 이 헤더는 모든 엔진 파일의 근본 의존이다. */

/*
 * [한국어] GlusterFS 엔진 공통 옵션 구조체.
 * 잡의 td->eo 가 이 구조체를 가리키며, 사용자가 잡 파일/CLI로 지정한 값이 채워진다.
 * 옵션 정의 자체(`fio_option gfapi_options[]`)는 `engines/glusterfs.c` 에 있다.
 */
struct gf_options {
	void *pad;
	/* [한국어] fio 옵션 시스템은 option->off1 이 0인 것을 "미사용" 으로 본다.
	 * 따라서 struct 맨 앞에 실제 필드를 두면 off1==0 충돌이 발생할 수 있어, 더미 패딩을 둔다.
	 * 설정자: 없음. 읽는 자: 없음(순수 자리 홀더).
	 * 값 범위: 항상 초기화되지 않은 포인터 크기 영역. 동기화: 없음. */

	char *gf_vol;
	/* [한국어] GlusterFS 볼륨 이름 — `gluster volume create` 로 생성된 논리 볼륨의 이름.
	 * 설정자: fio 옵션 파서(option table의 off1). 잡 초기화 후 불변.
	 * 읽는 자: fio_gf_setup()이 glfs_new(volname) 에 전달.
	 * 값 범위: 비-NULL 문자열(옵션 미지정 시 엔진 init 단계에서 에러 처리).
	 * 동기화: 불변이라 없음. */

	char *gf_brick;
	/* [한국어] 볼륨 정보를 받아올 서버(브릭) 주소 — `gluster volume info` 의 노드 중 하나.
	 * 설정자: 옵션 파서. 잡 초기화 후 불변.
	 * 읽는 자: fio_gf_setup()이 glfs_set_volfile_server(fs, "tcp", brick, 24007) 에 전달.
	 * 값 범위: IP 또는 FQDN (비-NULL). 포트는 libgfapi 기본값 24007.
	 * 동기화: 불변이라 없음. */

	int gf_single_instance;
	/* [한국어] 1이면 동일 볼륨에 대해 glfs_t 핸들을 프로세스 내 여러 잡이 공유(참조 카운트 관리).
	 * 0이면 각 잡이 독립적인 glfs_t 를 생성(MON 연결/캐시 메타데이터 중복).
	 * 설정자: 옵션 파서. 읽는 자: fio_gf_setup()/cleanup()이 hash table 룩업 시 검사.
	 * 값 범위: 0/1. 동기화: 해시 조회·참조 카운트 변경에 pthread_mutex 사용(glusterfs.c 내부). */
};

/*
 * [한국어] GlusterFS 엔진의 파일별 실행 상태.
 * 잡 스레드 1명만 접근하며, FILE_SET_ENG_DATA(f, gf_data) 로 fio_file 에 부착된다.
 * 비동기 엔진의 경우 aio_events 에 완료된 io_u 포인터가 순차로 수집된다.
 */
struct gf_data {
	glfs_t *fs;
	/* [한국어] GlusterFS 마운트 핸들 — glfs_new+init 로 얻는 볼륨 단위 객체.
	 * 설정자: fio_gf_setup(). 읽는 자: 거의 모든 glfs_* 호출.
	 * 값 범위: 유효 포인터(마운트 성공 후) 또는 NULL(설정 실패 시).
	 * 동기화: libgfapi 내부 락 사용. single_instance=1 일 때는 여러 잡이 공유(참조 카운트). */

	glfs_fd_t *fd;
	/* [한국어] 열린 GlusterFS 파일 디스크립터 — glfs_open/creat 반환값.
	 * 설정자: fio_gf_open_file(). 읽는 자: queue()에서 glfs_pread/pwrite 변종에 전달.
	 * 값 범위: 유효 포인터(오픈 이후) 또는 NULL(오픈 실패/미오픈).
	 * 동기화: libgfapi 내부 관리. 잡 스레드 단독 소유. */

	struct io_u **aio_events;
	/* [한국어] 비동기 엔진 전용 — 완료된 io_u 포인터 링 버퍼.
	 * 설정자: glusterfs_async 엔진의 콜백(gf_async_cb)이 인덱스에 저장.
	 * 읽는 자: fio_gf_event()/getevents()가 콜백 수확 후 fio 코어에 반환.
	 * 값 범위: td->o.iodepth 크기의 배열; 각 원소는 유효 io_u 포인터 또는 NULL.
	 * 동기화: 콜백은 libgfapi 스레드풀 스레드에서 호출 — 내부에서 원자적 카운터 사용. */
};

extern struct fio_option gfapi_options[];
/* [한국어] 세 gfapi 엔진이 공유하는 옵션 테이블(gf_options 구조 매핑).
 * 정의 위치: engines/glusterfs.c. gf_vol/gf_brick/gf_single_instance 세 엔트리 + NULL sentinel.
 * 엔진 쪽은 ioengine_ops.options = gfapi_options 로 바로 참조한다. */

extern int fio_gf_setup(struct thread_data *td);
/* [한국어] 잡당 1회 setup 콜백 — glfs_new→glfs_set_volfile_server→glfs_init.
 * single_instance=1 이면 프로세스 내 hash table에서 기존 glfs_t 재사용.
 * 반환: 0 성공, 에러 시 음수/양수(엔진 규약대로). 호출자: fio_backend 잡 초기화. */

extern void fio_gf_cleanup(struct thread_data *td);
/* [한국어] setup의 역순 — glfs_fini 호출로 서버 연결 종료.
 * single_instance 참조 카운트가 0이 될 때만 실제 해제. 잡 종료 후 1회 호출. */

extern int fio_gf_get_file_size(struct thread_data *td, struct fio_file *f);
/* [한국어] glfs_stat 으로 파일 크기를 조회, f->real_file_size 에 기록.
 * fio 코어가 offset/블록 인덱싱에 사용하는 결정적 정보. open_file 전에 호출된다. */

extern int fio_gf_open_file(struct thread_data *td, struct fio_file *f);
/* [한국어] 파일당 1회 — glfs_open/creat. rw 모드는 td->o 옵션에 따라 결정.
 * 성공 시 gf_data 할당 + FILE_SET_ENG_DATA 부착. 반환 0 성공, !=0 실패(fio 규약). */

extern int fio_gf_close_file(struct thread_data *td, struct fio_file *f);
/* [한국어] open_file 역순 — glfs_close 후 gf_data 해제, 슬롯 NULL 정리. */

extern int fio_gf_unlink_file(struct thread_data *td, struct fio_file *f);
/* [한국어] 잡 종료 시 생성된 파일 제거(create_on_open 모드 등) — glfs_unlink. */
