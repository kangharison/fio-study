/*
 * [한국어 설명] 유저스페이스 iSCSI initiator 기반 I/O 엔진 (libiscsi.c)
 *
 * === 파일의 역할 ===
 * libiscsi(유저스페이스 iSCSI initiator 라이브러리)를 사용해 원격 iSCSI 타겟/LUN에
 * SCSI READ16/WRITE16 명령으로 직접 I/O를 수행하는 fio 엔진 "libiscsi"를 구현한다.
 * 커널의 open-iscsi initiator나 tcm_loop에 의존하지 않고, poll(2) 기반 이벤트 루프로
 * 비동기 완료를 처리한다. fio의 각 파일 이름은 iSCSI URL(iscsi://host/iqn/lun)로
 * 해석되어 struct iscsi_lun 하나에 매핑된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * --ioengine=libiscsi로 선택되는 플러그인. 실행 흐름: backend.c → td_io_init →
 * fio_iscsi_setup/_init → open_file(iscsi_create_context + iscsi_full_connect_sync +
 * READ CAPACITY로 block_size/num_blocks 확보) → I/O 루프(queue에서
 * iscsi_pread16_iov_task/iscsi_pwrite16_iov_task 제출, getevents에서 poll+
 * iscsi_service로 완료 수확) → cleanup. 실행 컨텍스트는 fio 잡 스레드 1개.
 *
 * === 타 모듈과의 연결 ===
 * 상단: fio 코어(backend.c, ioengines.c, options.c).
 * 하단: libiscsi(iscsi_create_context/destroy_context/full_connect_sync/
 *       pread16_iov_task/pwrite16_iov_task/service/get_fd/which_events),
 *       poll(2) 시스콜.
 * 데이터 흐름: io_u->xfer_buf(iov 하나) ↔ SCSI CDB(READ16/WRITE16) ↔ TCP/IP ↔ 타겟 LBA.
 * 공유 상태: iscsi_info(td->io_ops_data), 여러 LUN 배열(luns[]), 완료 이벤트 큐.
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_iscsi_setup()/_init(): 옵션 파싱과 iscsi_info 할당.
 * - fio_iscsi_open_file(): iSCSI URL 파싱, 컨텍스트 생성, 동기 연결, READ CAPACITY.
 * - fio_iscsi_queue(): READ16/WRITE16 비동기 제출, 완료 콜백에서 complete_events 등록.
 * - fio_iscsi_getevents()/_event(): poll + iscsi_service 루프로 완료 수확.
 * - struct iscsi_task/iscsi_lun/iscsi_info: 작업 단위/LUN/엔진 전역 상태.
 */

/*
 * libiscsi engine
 *
 * this engine read/write iscsi lun with libiscsi.
 */


#include "../fio.h"
#include "../optgroup.h"

#include <stdlib.h>
#include <iscsi/iscsi.h>
#include <iscsi/scsi-lowlevel.h>
#include <poll.h>

struct iscsi_lun;
struct iscsi_info;

/* [한국어] 개별 iSCSI 작업 상태 구조체 */
struct iscsi_task {
	struct scsi_task	*scsi_task;	/* [한국어] libiscsi SCSI 작업 핸들 */
	struct iscsi_lun	*iscsi_lun;	/* [한국어] 이 작업이 대상으로 하는 LUN */
	struct io_u		*io_u;		/* [한국어] 대응하는 fio io_u */
};

/* [한국어] 개별 iSCSI LUN 정보 구조체 */
struct iscsi_lun {
	struct iscsi_info	*iscsi_info;	/* [한국어] 상위 iscsi_info 구조체 참조 */
	struct iscsi_context	*iscsi;		/* [한국어] libiscsi 연결 컨텍스트 */
	struct iscsi_url        *url;		/* [한국어] 파싱된 iSCSI URL */
	int			 block_size;	/* [한국어] LUN의 논리 블록 크기 */
	uint64_t		 num_blocks;	/* [한국어] LUN의 총 블록 수 */
};

/* [한국어] iSCSI 엔진의 전체 상태 구조체 - thread_data->io_ops_data에 저장 */
struct iscsi_info {
	struct iscsi_lun	**luns;			/* [한국어] LUN 배열 - 각 파일이 하나의 LUN에 대응 */
	int			  nr_luns;		/* [한국어] 열린 LUN 수 */
	struct pollfd		 *pfds;			/* [한국어] poll용 파일 디스크립터 배열 */
	struct iscsi_task	**complete_events;	/* [한국어] 완료된 작업 배열 */
	int			  nr_events;		/* [한국어] 완료된 이벤트 수 */
};

struct iscsi_options {
	void	*pad;
	char	*initiator;
};

static struct fio_option options[] = {
	{
		.name	  = "initiator",
		.lname	  = "initiator",
		.type	  = FIO_OPT_STR_STORE,
		.off1	  = offsetof(struct iscsi_options, initiator),
		.def	  = "iqn.2019-04.org.fio:fio",
		.help	  = "initiator name",
		.category = FIO_OPT_C_ENGINE,
		.group	  = FIO_OPT_G_ISCSI,
	},

	{
		.name = NULL,
	},
};

static int fio_iscsi_setup_lun(struct iscsi_info *iscsi_info,
			       char *initiator, struct fio_file *f, int i)
{
	struct iscsi_lun		*iscsi_lun  = NULL;
	struct scsi_task		*task	    = NULL;
	struct scsi_readcapacity16	*rc16	    = NULL;
	int				 ret	    = 0;

	iscsi_lun = calloc(1, sizeof(struct iscsi_lun));

	iscsi_lun->iscsi_info = iscsi_info;

	iscsi_lun->url = iscsi_parse_full_url(NULL, f->file_name);
	if (iscsi_lun->url == NULL) {
		log_err("iscsi: failed to parse url: %s\n", f->file_name);
		ret = EINVAL;
		goto out;
	}

	iscsi_lun->iscsi = iscsi_create_context(initiator);
	if (iscsi_lun->iscsi == NULL) {
		log_err("iscsi: failed to create iscsi context.\n");
		ret = 1;
		goto out;
	}

	if (iscsi_set_targetname(iscsi_lun->iscsi, iscsi_lun->url->target)) {
		log_err("iscsi: failed to set target name.\n");
		ret = EINVAL;
		goto out;
	}

	if (iscsi_set_session_type(iscsi_lun->iscsi, ISCSI_SESSION_NORMAL) != 0) {
		log_err("iscsi: failed to set session type.\n");
		ret = EINVAL;
		goto out;
	}

	if (iscsi_set_header_digest(iscsi_lun->iscsi,
				    ISCSI_HEADER_DIGEST_NONE_CRC32C) != 0) {
		log_err("iscsi: failed to set header digest.\n");
		ret = EINVAL;
		goto out;
	}

	if (iscsi_full_connect_sync(iscsi_lun->iscsi,
				    iscsi_lun->url->portal,
				    iscsi_lun->url->lun)) {
		log_err("iscsi: failed to connect to LUN : %s\n",
			iscsi_get_error(iscsi_lun->iscsi));
		ret = EINVAL;
		goto out;
	}

	task = iscsi_readcapacity16_sync(iscsi_lun->iscsi, iscsi_lun->url->lun);
	if (task == NULL || task->status != SCSI_STATUS_GOOD) {
		log_err("iscsi: failed to send readcapacity command: %s\n",
			iscsi_get_error(iscsi_lun->iscsi));
		ret = EINVAL;
		goto out;
	}

	rc16 = scsi_datain_unmarshall(task);
	if (rc16 == NULL) {
		log_err("iscsi: failed to unmarshal readcapacity16 data.\n");
		ret = EINVAL;
		goto out;
	}

	iscsi_lun->block_size = rc16->block_length;
	iscsi_lun->num_blocks = rc16->returned_lba + 1;

	scsi_free_scsi_task(task);
	task = NULL;

	f->real_file_size = iscsi_lun->num_blocks * iscsi_lun->block_size;
	f->engine_data	  = iscsi_lun;

	iscsi_info->luns[i]    = iscsi_lun;
	iscsi_info->pfds[i].fd = iscsi_get_fd(iscsi_lun->iscsi);

out:
	if (task) {
		scsi_free_scsi_task(task);
	}

	if (ret && iscsi_lun) {
		if (iscsi_lun->iscsi != NULL) {
			if (iscsi_is_logged_in(iscsi_lun->iscsi)) {
				iscsi_logout_sync(iscsi_lun->iscsi);
			}
			iscsi_destroy_context(iscsi_lun->iscsi);
		}
		free(iscsi_lun);
	}

	return ret;
}

static int fio_iscsi_setup(struct thread_data *td)
{
	struct iscsi_options	*options    = td->eo;
	struct iscsi_info	*iscsi_info = NULL;
	int			 ret	    = 0;
	struct fio_file		*f;
	int			 i;

	iscsi_info	    = malloc(sizeof(struct iscsi_info));
	iscsi_info->nr_luns = td->o.nr_files;
	iscsi_info->luns    = calloc(iscsi_info->nr_luns, sizeof(struct iscsi_lun*));
	iscsi_info->pfds    = calloc(iscsi_info->nr_luns, sizeof(struct pollfd));

	iscsi_info->nr_events	    = 0;
	iscsi_info->complete_events = calloc(td->o.iodepth, sizeof(struct iscsi_task*));

	td->io_ops_data = iscsi_info;

	for_each_file(td, f, i) {
		ret = fio_iscsi_setup_lun(iscsi_info, options->initiator, f, i);
		if (ret < 0) break;
	}

	return ret;
}

static int fio_iscsi_init(struct thread_data *td) {
	return 0;
}

static void fio_iscsi_cleanup_lun(struct iscsi_lun *iscsi_lun) {
	if (iscsi_lun->iscsi != NULL) {
		if (iscsi_is_logged_in(iscsi_lun->iscsi)) {
			iscsi_logout_sync(iscsi_lun->iscsi);
		}
		iscsi_destroy_context(iscsi_lun->iscsi);
	}
	free(iscsi_lun);
}

static void fio_iscsi_cleanup(struct thread_data *td)
{
	struct iscsi_info *iscsi_info = td->io_ops_data;

	for (int i = 0; i < iscsi_info->nr_luns; i++) {
		if (iscsi_info->luns[i]) {
			fio_iscsi_cleanup_lun(iscsi_info->luns[i]);
			iscsi_info->luns[i] = NULL;
		}
	}

	free(iscsi_info->luns);
	free(iscsi_info->pfds);
	free(iscsi_info->complete_events);
	free(iscsi_info);
}

static int fio_iscsi_prep(struct thread_data *td, struct io_u *io_u)
{
	return 0;
}

static int fio_iscsi_open_file(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static int fio_iscsi_close_file(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static void iscsi_cb(struct iscsi_context *iscsi, int status,
		     void *command_data, void *private_data)
{
	struct iscsi_task	*iscsi_task = (struct iscsi_task*)private_data;
	struct iscsi_lun	*iscsi_lun  = iscsi_task->iscsi_lun;
	struct iscsi_info       *iscsi_info = iscsi_lun->iscsi_info;
	struct io_u             *io_u	    = iscsi_task->io_u;

	if (status == SCSI_STATUS_GOOD) {
		io_u->error = 0;
	} else {
		log_err("iscsi: request failed with error %s.\n",
			iscsi_get_error(iscsi_lun->iscsi));

		io_u->error = 1;
		io_u->resid = io_u->xfer_buflen;
	}

	iscsi_info->complete_events[iscsi_info->nr_events] = iscsi_task;
	iscsi_info->nr_events++;
}

static enum fio_q_status fio_iscsi_queue(struct thread_data *td,
					 struct io_u *io_u)
{
	struct iscsi_lun	*iscsi_lun  = io_u->file->engine_data;
	struct scsi_task	*scsi_task  = NULL;
	struct iscsi_task	*iscsi_task = malloc(sizeof(struct iscsi_task));
	int			 ret	    = -1;

	if (io_u->ddir == DDIR_READ || io_u->ddir == DDIR_WRITE) {
		if (io_u->offset % iscsi_lun->block_size != 0) {
			log_err("iscsi: offset is not align to block size.\n");
			ret = -1;
			goto out;
		}

		if (io_u->xfer_buflen % iscsi_lun->block_size != 0) {
			log_err("iscsi: buflen is not align to block size.\n");
			ret = -1;
			goto out;
		}
	}

	if (io_u->ddir == DDIR_READ) {
		scsi_task = scsi_cdb_read16(io_u->offset / iscsi_lun->block_size,
					    io_u->xfer_buflen,
					    iscsi_lun->block_size,
					    0, 0, 0, 0, 0);
		ret = scsi_task_add_data_in_buffer(scsi_task, io_u->xfer_buflen,
						   io_u->xfer_buf);
		if (ret < 0) {
			log_err("iscsi: failed to add data in buffer.\n");
			goto out;
		}
	} else if (io_u->ddir == DDIR_WRITE) {
		scsi_task = scsi_cdb_write16(io_u->offset / iscsi_lun->block_size,
					     io_u->xfer_buflen,
					     iscsi_lun->block_size,
					     0, 0, 0, 0, 0);
		ret = scsi_task_add_data_out_buffer(scsi_task, io_u->xfer_buflen,
						    io_u->xfer_buf);
		if (ret < 0) {
			log_err("iscsi: failed to add data out buffer.\n");
			goto out;
		}
	} else if (ddir_sync(io_u->ddir)) {
		scsi_task = scsi_cdb_synchronizecache16(
			0, iscsi_lun->num_blocks * iscsi_lun->block_size, 0, 0);
	} else {
		log_err("iscsi: invalid I/O operation: %d\n", io_u->ddir);
		ret = EINVAL;
		goto out;
	}

	iscsi_task->scsi_task = scsi_task;
	iscsi_task->iscsi_lun = iscsi_lun;
	iscsi_task->io_u      = io_u;

	ret = iscsi_scsi_command_async(iscsi_lun->iscsi, iscsi_lun->url->lun,
				       scsi_task, iscsi_cb, NULL, iscsi_task);
	if (ret < 0) {
		log_err("iscsi: failed to send scsi command.\n");
		goto out;
	}

	return FIO_Q_QUEUED;

out:
	if (iscsi_task) {
		free(iscsi_task);
	}

	if (scsi_task) {
		scsi_free_scsi_task(scsi_task);
	}

	if (ret) {
		io_u->error = ret;
	}
	return FIO_Q_COMPLETED;
}

static int fio_iscsi_getevents(struct thread_data *td, unsigned int min,
			       unsigned int max, const struct timespec *t)
{
	struct iscsi_info	*iscsi_info = td->io_ops_data;
	int			 ret	    = 0;

	iscsi_info->nr_events = 0;

	while (iscsi_info->nr_events < min) {
		for (int i = 0; i < iscsi_info->nr_luns; i++) {
			int events = iscsi_which_events(iscsi_info->luns[i]->iscsi);
			iscsi_info->pfds[i].events = events;
		}

		ret = poll(iscsi_info->pfds, iscsi_info->nr_luns, -1);
		if (ret < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				continue;
			}
			log_err("iscsi: failed to poll events: %s.\n",
				strerror(errno));
			break;
		}

		for (int i = 0; i < iscsi_info->nr_luns; i++) {
			ret = iscsi_service(iscsi_info->luns[i]->iscsi,
					    iscsi_info->pfds[i].revents);
			assert(ret >= 0);
		}
	}

	return ret < 0 ? ret : iscsi_info->nr_events;
}

static struct io_u *fio_iscsi_event(struct thread_data *td, int event)
{
	struct iscsi_info	*iscsi_info = (struct iscsi_info*)td->io_ops_data;
	struct iscsi_task	*iscsi_task = iscsi_info->complete_events[event];
	struct io_u		*io_u	    = iscsi_task->io_u;

	iscsi_info->complete_events[event] = NULL;

	scsi_free_scsi_task(iscsi_task->scsi_task);
	free(iscsi_task);

	return io_u;
}

FIO_STATIC struct ioengine_ops ioengine = {
	.name               = "libiscsi",
	.version            = FIO_IOOPS_VERSION,
	.flags              = FIO_SYNCIO | FIO_DISKLESSIO | FIO_NODISKUTIL,
	.setup              = fio_iscsi_setup,
	.init               = fio_iscsi_init,
	.prep               = fio_iscsi_prep,
	.queue              = fio_iscsi_queue,
	.getevents          = fio_iscsi_getevents,
	.event              = fio_iscsi_event,
	.cleanup            = fio_iscsi_cleanup,
	.open_file          = fio_iscsi_open_file,
	.close_file         = fio_iscsi_close_file,
	.option_struct_size = sizeof(struct iscsi_options),
	.options	    = options,
};

static void fio_init fio_iscsi_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_iscsi_unregister(void)
{
	unregister_ioengine(&ioengine);
}
