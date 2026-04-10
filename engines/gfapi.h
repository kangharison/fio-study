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

struct gf_options {
	void *pad;
	char *gf_vol;
	char *gf_brick;
	int gf_single_instance;
};

struct gf_data {
	glfs_t *fs;
	glfs_fd_t *fd;
	struct io_u **aio_events;
};

extern struct fio_option gfapi_options[];
extern int fio_gf_setup(struct thread_data *td);
extern void fio_gf_cleanup(struct thread_data *td);
extern int fio_gf_get_file_size(struct thread_data *td, struct fio_file *f);
extern int fio_gf_open_file(struct thread_data *td, struct fio_file *f);
extern int fio_gf_close_file(struct thread_data *td, struct fio_file *f);
extern int fio_gf_unlink_file(struct thread_data *td, struct fio_file *f);
