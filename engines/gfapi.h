/*
 * [한국어 설명] GlusterFS API (gfapi) 공통 헤더 (gfapi.h)
 *
 * === 개요 ===
 * GlusterFS 분산 파일 시스템의 gfapi 기반 I/O 엔진들이 공유하는 헤더 파일이다.
 * GlusterFS 볼륨/브릭 설정 옵션(gf_options)과 연결 상태 데이터(gf_data)를 정의하며,
 * setup, cleanup, open, close 등 공통 콜백 함수를 선언한다.
 *
 * === 사용하는 라이브러리 ===
 * libgfapi (glusterfs/api/glfs.h)
 */

#include <glusterfs/api/glfs.h>
#include "../fio.h"

/* [한국어] GlusterFS 엔진 전용 옵션 구조체 */
struct gf_options {
	void *pad;			/* [한국어] fio 옵션 구조체 정렬 패딩 */
	char *gf_vol;			/* [한국어] GlusterFS 볼륨 이름 */
	char *gf_brick;			/* [한국어] 연결할 GlusterFS 브릭 주소 */
	int gf_single_instance;		/* [한국어] 동일 볼륨에 대해 glfs 인스턴스를 공유할지 여부 */
};

/* [한국어] GlusterFS 엔진의 내부 상태 구조체 */
struct gf_data {
	glfs_t *fs;			/* [한국어] GlusterFS 파일 시스템 핸들 */
	glfs_fd_t *fd;			/* [한국어] 열린 GlusterFS 파일 디스크립터 */
	struct io_u **aio_events;	/* [한국어] 비동기 모드에서 완료된 io_u 배열 */
};

extern struct fio_option gfapi_options[];
extern int fio_gf_setup(struct thread_data *td);
extern void fio_gf_cleanup(struct thread_data *td);
extern int fio_gf_get_file_size(struct thread_data *td, struct fio_file *f);
extern int fio_gf_open_file(struct thread_data *td, struct fio_file *f);
extern int fio_gf_close_file(struct thread_data *td, struct fio_file *f);
extern int fio_gf_unlink_file(struct thread_data *td, struct fio_file *f);
