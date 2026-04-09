#ifndef FIO_CGROUP_H
#define FIO_CGROUP_H
/*
 * [한국어] cgroup.h - Linux cgroup(blkio) 지원 헤더
 *
 * cgroup을 통한 I/O 대역폭 제한 및 CPU 격리를 위한 인터페이스를 정의한다.
 * FIO_HAVE_CGROUPS가 정의된 경우 실제 구현을 사용하고,
 * 그렇지 않으면 스텁(stub) 함수가 제공된다.
 */

#ifdef FIO_HAVE_CGROUPS

/* [한국어] cgroup 마운트 포인트 정보 구조체 */
struct cgroup_mnt {
	char *path;       /* cgroup 마운트 경로 (예: /sys/fs/cgroup/blkio) */
	bool cgroup2;     /* true이면 cgroup v2 사용 */
};

int cgroup_setup(struct thread_data *, struct flist_head *, struct cgroup_mnt **);
void cgroup_shutdown(struct thread_data *, struct cgroup_mnt *);

void cgroup_kill(struct flist_head *list);

#else

/* [한국어] cgroup 미지원 환경을 위한 스텁 구현 */
struct cgroup_mnt;

static inline int cgroup_setup(struct thread_data *td, struct flist_head *list,
			       struct cgroup_mnt **mnt)
{
	td_verror(td, EINVAL, "cgroup_setup");
	return 1;
}

static inline void cgroup_shutdown(struct thread_data *td, struct cgroup_mnt *mnt)
{
}

static inline void cgroup_kill(struct flist_head *list)
{
}

#endif
#endif
