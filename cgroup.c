/*
 * Code related to setting up a blkio cgroup
 */
/*
 * [한국어] cgroup.c - Linux cgroup(blkio) 설정 및 관리
 *
 * 이 파일은 fio 작업(job)을 Linux cgroup의 blkio 컨트롤러에 할당하는 기능을 구현한다.
 * 주요 기능:
 *   1) cgroup_setup()    - cgroup 디렉토리 생성 및 작업 PID 등록
 *   2) cgroup_shutdown() - cgroup에서 PID 제거 및 자원 해제
 *   3) cgroup_kill()     - cgroup 디렉토리 삭제 및 멤버 리스트 정리
 *
 * cgroup v1(blkio)과 cgroup v2 모두 지원하며,
 * /proc/mounts에서 마운트 포인트를 자동 탐지한다.
 * blkio.weight를 통해 I/O 가중치 설정이 가능하다 (cgroup v1 전용).
 */
#include <stdio.h>
#include <stdlib.h>
#include <mntent.h>
#include <sys/stat.h>
#include "fio.h"
#include "flist.h"
#include "cgroup.h"
#include "smalloc.h"

/* [한국어] cgroup 멤버 리스트 보호를 위한 세마포어 */
static struct fio_sem *lock;

/* [한국어] cgroup 멤버 구조체 - cgroup 디렉토리 경로와 삭제 정책을 저장 */
struct cgroup_member {
	struct flist_head list;          /* 연결 리스트 노드 */
	char *root;                      /* cgroup 디렉토리 경로 */
	unsigned int cgroup_nodelete;    /* 1이면 종료 시 디렉토리를 삭제하지 않음 */
};

/* [한국어] cgroup 마운트 포인트를 /proc/mounts에서 찾는 함수
 * cgroup v1(blkio)과 cgroup v2를 모두 탐색한다. */
static struct cgroup_mnt *find_cgroup_mnt(struct thread_data *td)
{
	struct cgroup_mnt *cgroup_mnt = NULL;
	struct mntent *mnt, dummy;
	char buf[256] = {0};
	FILE *f;
	bool cgroup2 = false;

	f = setmntent("/proc/mounts", "r");
	if (!f) {
		td_verror(td, errno, "setmntent /proc/mounts");
		return NULL;
	}

	/* /proc/mounts를 순회하며 cgroup(blkio) 또는 cgroup2 마운트 탐색 */
	while ((mnt = getmntent_r(f, &dummy, buf, sizeof(buf))) != NULL) {
		if (!strcmp(mnt->mnt_type, "cgroup") &&
		    strstr(mnt->mnt_opts, "blkio"))
			break;
		if (!strcmp(mnt->mnt_type, "cgroup2")) {
			cgroup2 = true;
			break;
		}
	}

	if (mnt) {
		/* 공유 메모리에 마운트 정보 할당 */
		cgroup_mnt = smalloc(sizeof(*cgroup_mnt));
		if (cgroup_mnt) {
			cgroup_mnt->path = smalloc_strdup(mnt->mnt_dir);
			if (!cgroup_mnt->path) {
				sfree(cgroup_mnt);
				log_err("fio: could not allocate memory\n");
			} else {
				cgroup_mnt->cgroup2 = cgroup2;
			}
		}
	} else {
		log_err("fio: cgroup blkio does not appear to be mounted\n");
	}

	endmntent(f);
	return cgroup_mnt;
}

/* [한국어] cgroup 멤버를 리스트에 추가하는 함수
 * 세마포어로 동기화하여 안전하게 리스트에 삽입한다. */
static void add_cgroup(struct thread_data *td, const char *name,
			struct flist_head *clist)
{
	struct cgroup_member *cm;

	if (!lock)
		return;

	cm = smalloc(sizeof(*cm));
	if (!cm) {
err:
		log_err("fio: failed to allocate cgroup member\n");
		return;
	}

	INIT_FLIST_HEAD(&cm->list);
	cm->root = smalloc_strdup(name);
	if (!cm->root) {
		sfree(cm);
		goto err;
	}
	if (td->o.cgroup_nodelete)
		cm->cgroup_nodelete = 1;
	fio_sem_down(lock);
	flist_add_tail(&cm->list, clist);
	fio_sem_up(lock);
}

/* [한국어] cgroup 멤버 리스트를 정리하고 디렉토리를 삭제하는 함수
 * cgroup_nodelete 설정이 없는 디렉토리만 rmdir로 삭제한다. */
void cgroup_kill(struct flist_head *clist)
{
	struct flist_head *n, *tmp;
	struct cgroup_member *cm;

	if (!lock)
		return;

	fio_sem_down(lock);

	flist_for_each_safe(n, tmp, clist) {
		cm = flist_entry(n, struct cgroup_member, list);
		if (!cm->cgroup_nodelete)
			rmdir(cm->root);
		flist_del(&cm->list);
		sfree(cm->root);
		sfree(cm);
	}

	fio_sem_up(lock);
}

/* [한국어] cgroup 디렉토리의 전체 경로를 생성하는 함수
 * 마운트포인트/cgroup이름 형태로 경로를 조합한다. */
static char *get_cgroup_root(struct thread_data *td, struct cgroup_mnt *mnt)
{
	char *str = malloc(64);

	if (td->o.cgroup)
		sprintf(str, "%s/%s", mnt->path, td->o.cgroup);
	else
		sprintf(str, "%s/%s", mnt->path, td->o.name);

	return str;
}

/* [한국어] 파일에 정수 값을 쓰는 헬퍼 함수
 * cgroup 제어 파일(예: blkio.weight, tasks)에 값을 기록한다. */
static int write_int_to_file(struct thread_data *td, const char *path,
			     const char *filename, unsigned int val,
			     const char *onerr)
{
	char tmp[256];
	FILE *f;

	sprintf(tmp, "%s/%s", path, filename);
	f = fopen(tmp, "w");
	if (!f) {
		td_verror(td, errno, onerr);
		return 1;
	}

	fprintf(f, "%u", val);
	fclose(f);
	return 0;

}

/* [한국어] cgroup에 현재 작업의 PID를 등록하는 함수
 * cgroup v2에서는 cgroup.procs, v1에서는 tasks 파일에 기록한다. */
static int cgroup_write_pid(struct thread_data *td, char *path, bool cgroup2)
{
	unsigned int val = td->pid;

	if (cgroup2)
		return write_int_to_file(td, path, "cgroup.procs",
					 val, "cgroup write pid");
	return write_int_to_file(td, path, "tasks", val, "cgroup write pid");
}

/*
 * Move pid to root class
 */
/* [한국어] PID를 최상위 cgroup으로 이동시키는 함수 (cgroup에서 제거) */
static int cgroup_del_pid(struct thread_data *td, struct cgroup_mnt *mnt)
{
	return cgroup_write_pid(td, mnt->path, mnt->cgroup2);
}

/* [한국어] cgroup 초기 설정 함수
 * 1) cgroup 마운트 포인트 탐색
 * 2) cgroup 디렉토리 생성 (이미 존재하면 건너뜀)
 * 3) blkio.weight 설정 (옵션 지정 시)
 * 4) 현재 작업 PID를 cgroup에 등록 */
int cgroup_setup(struct thread_data *td, struct flist_head *clist, struct cgroup_mnt **mnt)
{
	char *root;

	if (!clist)
		return 1;

	if (!*mnt) {
		*mnt = find_cgroup_mnt(td);
		if (!*mnt)
			return 1;
	}

	/*
	 * Create container, if it doesn't exist
	 */
	root = get_cgroup_root(td, *mnt);
	if (mkdir(root, 0755) < 0) {
		int __e = errno;

		if (__e != EEXIST) {
			td_verror(td, __e, "cgroup mkdir");
			log_err("fio: path %s\n", root);
			goto err;
		}
	} else
		add_cgroup(td, root, clist);

	if (td->o.cgroup_weight) {
		if ((*mnt)->cgroup2) {
			log_err("fio: cgroup weit doesn't work with cgroup2\n");
			goto err;
		}
		if (write_int_to_file(td, root, "blkio.weight",
					td->o.cgroup_weight,
					"cgroup open weight"))
			goto err;
	}

	if (!cgroup_write_pid(td, root, (*mnt)->cgroup2)) {
		free(root);
		return 0;
	}

err:
	free(root);
	return 1;
}

/* [한국어] cgroup 종료 정리 함수
 * PID를 최상위 cgroup으로 이동시키고, 마운트 정보 메모리를 해제한다. */
void cgroup_shutdown(struct thread_data *td, struct cgroup_mnt *mnt)
{
	if (mnt == NULL)
		return;
	if (!td->o.cgroup_weight && !td->o.cgroup)
		goto out;

	cgroup_del_pid(td, mnt);
out:
	if (mnt->path)
		sfree(mnt->path);
	sfree(mnt);
}

/* [한국어] fio 초기화 시 cgroup 세마포어를 생성하는 함수 (fio_init 속성) */
static void fio_init cgroup_init(void)
{
	lock = fio_sem_init(FIO_SEM_UNLOCKED);
	if (!lock)
		log_err("fio: failed to allocate cgroup lock\n");
}

/* [한국어] fio 종료 시 cgroup 세마포어를 해제하는 함수 (fio_exit 속성) */
static void fio_exit cgroup_exit(void)
{
	fio_sem_remove(lock);
}
