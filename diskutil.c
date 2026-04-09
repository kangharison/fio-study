/*
 * [한국어] diskutil.c - 디스크 유틸리티 통계 수집
 *
 * /sys/block/<dev>/stat에서 디스크 I/O 활용도를 주기적으로 수집하는 기능을 구현한다.
 * iostat과 유사한 통계(읽기/쓰기 IOPS, 섹터, 병합, 틱 등)를 fio 실행 중에 제공한다.
 *
 * 주요 기능:
 *   1) init_disk_util()      - 파일에 해당하는 디바이스를 찾아 디스크 유틸 항목 생성
 *   2) update_io_ticks()     - 모든 등록된 디바이스의 통계를 폴링하여 갱신
 *   3) disk_util_add()       - 새 디바이스를 전역 disk_list에 등록
 *   4) find_add_disk_slaves()- 소프트웨어 RAID의 슬레이브 디바이스를 탐색 및 등록
 *   5) disk_util_prune_entries() - 전역 리스트에서 모든 항목을 제거
 *   6) setup_disk_util()     - 전역 세마포어 초기화
 *
 * 디바이스 탐색 흐름:
 *   파일 경로 -> stat()으로 major/minor 번호 획득 -> /sys/block/ 탐색 ->
 *   sysfs에서 파티션/디스크 구분 -> disk_util 구조체 생성 및 등록
 */

#include <inttypes.h>       /* PRIu64 등 고정 크기 정수 포맷 매크로 */
#include <stdio.h>          /* 파일 I/O */
#include <string.h>         /* 문자열 처리 */
#include <sys/types.h>      /* 시스템 자료형 */
#include <sys/stat.h>       /* 파일 상태 조회 */
#include <sys/sysmacros.h>  /* major(), minor() 매크로 */
#include <dirent.h>         /* 디렉토리 탐색 */
#include <libgen.h>         /* dirname(), basename() */
#ifdef CONFIG_VALGRIND_DEV
#include <valgrind/drd.h>
#else
#define DRD_IGNORE_VAR(x) do { } while (0)
#endif

#include "fio.h"            /* fio 핵심 구조체 및 매크로 */
#include "smalloc.h"        /* 공유 메모리 할당기 */
#include "diskutil.h"       /* 디스크 유틸리티 헤더 */
#include "helper_thread.h"  /* 헬퍼 스레드 인터페이스 */

/* [한국어] 캐싱용 전역 변수 - 마지막으로 조회한 디바이스의 major/minor를 저장하여
 * 동일 디바이스에 대한 반복적인 sysfs 탐색을 방지한다. */
static int last_majdev, last_mindev;
static struct disk_util *last_du;

static struct fio_sem *disk_util_sem;  /* 전역 디스크 유틸 리스트 보호용 세마포어 */

static struct disk_util *__init_per_file_disk_util(struct thread_data *td,
		int majdev, int mindev, char *path);

/* [한국어] disk_util 구조체 해제 - 슬레이브 연결을 끊고 리소스를 반환 */
static void disk_util_free(struct disk_util *du)
{
	if (du == last_du)
		last_du = NULL;

	/* 슬레이브 리스트에서 모든 슬레이브를 분리 */
	while (!flist_empty(&du->slaves)) {
		struct disk_util *slave;

		slave = flist_first_entry(&du->slaves, struct disk_util, slavelist);
		flist_del(&slave->slavelist);
		slave->users--;
	}

	fio_shared_sem_remove(du->lock);
	free(du->sysfs_root);
	sfree(du);
}

/* [한국어] 디바이스의 I/O 통계 읽기 - /sys/block/<dev>/stat 파일을 파싱
 * sscanf()로 읽기/쓰기의 ios, merges, sectors, ticks와 io_ticks, time_in_queue를 추출한다.
 * 반환값: 0=성공, 1=실패 */
static int get_io_ticks(struct disk_util *du, struct disk_util_stat *dus)
{
	char line[256];
	FILE *f;
	char *p;
	int ret;

	dprint(FD_DISKUTIL, "open stat file: %s\n", du->path);

	f = fopen(du->path, "r");
	if (!f)
		return 1;

	p = fgets(line, sizeof(line), f);
	if (!p) {
		fclose(f);
		return 1;
	}

	dprint(FD_DISKUTIL, "%s: %s", du->path, p);

	ret = sscanf(p, "%"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" "
		     "%"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" "
		     "%*u %"SCNu64" %"SCNu64"\n",
		     &dus->s.ios[0], &dus->s.merges[0], &dus->s.sectors[0],
		     &dus->s.ticks[0],
		     &dus->s.ios[1], &dus->s.merges[1], &dus->s.sectors[1],
		     &dus->s.ticks[1],
		     &dus->s.io_ticks, &dus->s.time_in_queue);
	fclose(f);
	dprint(FD_DISKUTIL, "%s: stat read ok? %d\n", du->path, ret == 10);
	return ret != 10;
}

/* [한국어] 32비트 오버플로우를 안전하게 처리하는 차이 계산
 * 리눅스 커널이 일부 통계 필드를 32비트로 출력하기 때문에,
 * 값이 32비트 경계에서 랩어라운드(wrap-around)되는 경우를 보정한다. */
static uint64_t safe_32bit_diff(uint64_t nval, uint64_t oval)
{
	/* Linux kernel prints some of the stat fields as 32-bit integers. It is
	 * possible that the value overflows, but since fio uses unsigned 64-bit
	 * arithmetic in update_io_tick_disk(), it instead results in a huge
	 * bogus value being added to the respective accumulating field. Just
	 * in case Linux starts reporting these metrics as 64-bit values in the
	 * future, check that overflow actually happens around the 32-bit
	 * unsigned boundary; assume overflow only happens once between
	 * successive polls.
	 */
	if (oval <= nval || oval >= (1ull << 32))
		return nval - oval;
	else
		return (1ull << 32) + nval - oval;
}

/* [한국어] 단일 디바이스의 I/O 통계 갱신 - 현재값과 이전값의 차이(델타)를 누적
 * sectors, ios, merges는 단순 차이를, ticks 계열은 safe_32bit_diff()로 안전하게 계산한다. */
static void update_io_tick_disk(struct disk_util *du)
{
	struct disk_util_stat __dus, *dus, *ldus;
	struct timespec t;

	if (!du->users)
		return;
	if (get_io_ticks(du, &__dus))
		return;

	dus = &du->dus;       /* 누적 통계 */
	ldus = &du->last_dus;  /* 이전 폴링 값 */

	/* 델타(현재 - 이전) 계산 후 누적 */
	dus->s.sectors[0] += (__dus.s.sectors[0] - ldus->s.sectors[0]);
	dus->s.sectors[1] += (__dus.s.sectors[1] - ldus->s.sectors[1]);
	dus->s.ios[0] += (__dus.s.ios[0] - ldus->s.ios[0]);
	dus->s.ios[1] += (__dus.s.ios[1] - ldus->s.ios[1]);
	dus->s.merges[0] += (__dus.s.merges[0] - ldus->s.merges[0]);
	dus->s.merges[1] += (__dus.s.merges[1] - ldus->s.merges[1]);
	dus->s.ticks[0] += safe_32bit_diff(__dus.s.ticks[0], ldus->s.ticks[0]);
	dus->s.ticks[1] += safe_32bit_diff(__dus.s.ticks[1], ldus->s.ticks[1]);
	dus->s.io_ticks += safe_32bit_diff(__dus.s.io_ticks, ldus->s.io_ticks);
	dus->s.time_in_queue +=
			safe_32bit_diff(__dus.s.time_in_queue, ldus->s.time_in_queue);

	/* 경과 시간 갱신 */
	fio_gettime(&t, NULL);
	dus->s.msec += mtime_since(&du->time, &t);
	du->time = t;
	ldus->s = __dus.s;  /* 현재 통계를 "이전"으로 저장 */
}

/* [한국어] 전체 등록 디바이스의 I/O 통계 갱신 - 헬퍼 스레드에서 주기적으로 호출
 * disk_list를 순회하며 각 디바이스의 update_io_tick_disk()를 호출한다.
 * 반환값: 0=정상, 1=헬퍼 스레드 종료 요청됨 */
int update_io_ticks(void)
{
	struct flist_head *entry;
	struct disk_util *du;
	int ret = 0;

	dprint(FD_DISKUTIL, "update io ticks\n");

	fio_sem_down(disk_util_sem);

	if (!helper_should_exit()) {
		flist_for_each(entry, &disk_list) {
			du = flist_entry(entry, struct disk_util, list);
			update_io_tick_disk(du);
		}
	} else
		ret = 1;

	fio_sem_up(disk_util_sem);
	return ret;
}

/* [한국어] major/minor 번호로 기존 disk_util 항목 검색 - 이미 등록된 디바이스인지 확인 */
static struct disk_util *disk_util_exists(int major, int minor)
{
	struct flist_head *entry;
	struct disk_util *du;

	fio_sem_down(disk_util_sem);

	flist_for_each(entry, &disk_list) {
		du = flist_entry(entry, struct disk_util, list);

		if (major == du->major && minor == du->minor) {
			fio_sem_up(disk_util_sem);
			return du;
		}
	}

	fio_sem_up(disk_util_sem);
	return NULL;
}

/* [한국어] 파일에서 major/minor 디바이스 번호 획득
 * 블록 디바이스이면 st_rdev, 일반 파일이면 st_dev에서 추출한다.
 * 문자 디바이스나 FIFO는 디스크 통계와 무관하므로 -1을 반환한다. */
static int get_device_numbers(char *file_name, int *maj, int *min)
{
	struct stat st;
	int majdev, mindev;
	char tempname[PATH_MAX], *p;

	if (!lstat(file_name, &st)) {
		if (S_ISBLK(st.st_mode)) {
			majdev = major(st.st_rdev);
			mindev = minor(st.st_rdev);
		} else if (S_ISCHR(st.st_mode) ||
			   S_ISFIFO(st.st_mode)) {
			return -1;
		} else {
			majdev = major(st.st_dev);
			mindev = minor(st.st_dev);
		}
	} else {
		/*
		 * must be a file, open "." in that path
		 */
		/* 파일이 존재하지 않으면 상위 디렉토리의 디바이스 번호를 사용 */
		snprintf(tempname, FIO_ARRAY_SIZE(tempname), "%s", file_name);
		p = dirname(tempname);
		if (stat(p, &st)) {
			perror("disk util stat");
			return -1;
		}

		majdev = major(st.st_dev);
		mindev = minor(st.st_dev);
	}

	*min = mindev;
	*maj = majdev;

	return 0;
}

/* [한국어] 블록 디바이스 dev 파일 읽기 - "major:minor" 형식의 내용을 파싱 */
static int read_block_dev_entry(char *path, int *maj, int *min)
{
	char line[256], *p;
	FILE *f;

	f = fopen(path, "r");
	if (!f) {
		perror("open path");
		return 1;
	}

	p = fgets(line, sizeof(line), f);
	fclose(f);

	if (!p)
		return 1;

	if (sscanf(p, "%u:%u", maj, min) != 2)
		return 1;

	return 0;
}

/* [한국어] 소프트웨어 RAID의 슬레이브 디바이스 탐색 및 등록
 * sysfs의 slaves/ 디렉토리를 읽어 하위 디바이스를 찾고,
 * 각 슬레이브를 disk_util로 등록하여 마스터의 slaves 리스트에 연결한다. */
static void find_add_disk_slaves(struct thread_data *td, char *path,
				 struct disk_util *masterdu)
{
	DIR *dirhandle = NULL;
	struct dirent *dirent = NULL;
	char slavesdir[PATH_MAX], temppath[PATH_MAX], slavepath[PATH_MAX];
	struct disk_util *slavedu = NULL;
	int majdev, mindev;
	ssize_t linklen;

	sprintf(slavesdir, "%s/%s", path, "slaves");
	dirhandle = opendir(slavesdir);
	if (!dirhandle)
		return;

	while ((dirent = readdir(dirhandle)) != NULL) {
		if (!strcmp(dirent->d_name, ".") ||
		    !strcmp(dirent->d_name, ".."))
			continue;

		nowarn_snprintf(temppath, sizeof(temppath), "%s/%s", slavesdir,
				dirent->d_name);
		/* Can we always assume that the slaves device entries
		 * are links to the real directories for the slave
		 * devices?
		 */
		/* 심볼릭 링크를 따라가 실제 슬레이브 디바이스 경로를 얻음 */
		linklen = readlink(temppath, slavepath, PATH_MAX - 1);
		if (linklen < 0) {
			perror("readlink() for slave device.");
			closedir(dirhandle);
			return;
		}
		slavepath[linklen] = '\0';

		/* 슬레이브의 dev 파일에서 major/minor 번호 읽기 */
		nowarn_snprintf(temppath, sizeof(temppath), "%s/%s/dev",
				slavesdir, slavepath);
		if (access(temppath, F_OK) != 0)
			nowarn_snprintf(temppath, sizeof(temppath),
					"%s/%s/device/dev", slavesdir,
					slavepath);
		if (read_block_dev_entry(temppath, &majdev, &mindev)) {
			perror("Error getting slave device numbers");
			closedir(dirhandle);
			return;
		}

		/*
		 * See if this maj,min already exists
		 */
		/* 이미 등록된 슬레이브인지 확인 */
		slavedu = disk_util_exists(majdev, mindev);
		if (slavedu)
			continue;

		/* 새 슬레이브를 등록하고 마스터의 slaves 리스트에 추가 */
		nowarn_snprintf(temppath, sizeof(temppath), "%s/%s", slavesdir,
				slavepath);
		__init_per_file_disk_util(td, majdev, mindev, temppath);
		slavedu = disk_util_exists(majdev, mindev);

		/* Should probably use an assert here. slavedu should
		 * always be present at this point. */
		if (slavedu) {
			slavedu->users++;
			flist_add_tail(&slavedu->slavelist, &masterdu->slaves);
		}
	}

	closedir(dirhandle);
}

/* [한국어] 새 디바이스를 디스크 유틸리티 리스트에 추가
 * disk_util 구조체를 공유 메모리(smalloc)에 할당하고 초기화한다.
 * 이미 동일 이름의 디바이스가 등록되어 있으면 기존 항목을 반환한다.
 * 등록 후 find_add_disk_slaves()로 슬레이브 디바이스도 탐색한다. */
static struct disk_util *disk_util_add(struct thread_data *td, int majdev,
				       int mindev, char *path)
{
	struct disk_util *du, *__du;
	struct flist_head *entry;
	int l;

	dprint(FD_DISKUTIL, "add maj/min %d/%d: %s\n", majdev, mindev, path);

	du = smalloc(sizeof(*du));
	if (!du)
		return NULL;

	DRD_IGNORE_VAR(du->users);
	memset(du, 0, sizeof(*du));
	INIT_FLIST_HEAD(&du->list);
	l = snprintf(du->path, sizeof(du->path), "%s/stat", path);
	if (l < 0 || l >= sizeof(du->path)) {
		log_err("constructed path \"%.100s[...]/stat\" larger than buffer (%zu bytes)\n",
			path, sizeof(du->path) - 1);
		sfree(du);
		return NULL;
	}
	snprintf((char *) du->dus.name, FIO_ARRAY_SIZE(du->dus.name), "%s",
		 basename(path));
	du->sysfs_root = strdup(path);
	du->major = majdev;
	du->minor = mindev;
	INIT_FLIST_HEAD(&du->slavelist);
	INIT_FLIST_HEAD(&du->slaves);
	du->lock = fio_shared_sem_init(FIO_SEM_UNLOCKED);
	du->users = 0;

	fio_sem_down(disk_util_sem);

	/* 동일 이름의 디바이스가 이미 존재하는지 확인 */
	flist_for_each(entry, &disk_list) {
		__du = flist_entry(entry, struct disk_util, list);

		dprint(FD_DISKUTIL, "found %s in list\n", __du->dus.name);

		if (!strcmp((char *) du->dus.name, (char *) __du->dus.name)) {
			disk_util_free(du);
			fio_sem_up(disk_util_sem);
			return __du;
		}
	}

	dprint(FD_DISKUTIL, "add %s to list\n", du->dus.name);

	/* 초기 통계 스냅샷 저장 */
	fio_gettime(&du->time, NULL);
	get_io_ticks(du, &du->last_dus);

	flist_add_tail(&du->list, &disk_list);
	fio_sem_up(disk_util_sem);

	/* 소프트웨어 RAID의 슬레이브 디바이스 탐색 */
	find_add_disk_slaves(td, path, du);
	return du;
}

/* [한국어] 디바이스 major/minor 번호 매칭 확인 */
static int check_dev_match(int majdev, int mindev, char *path)
{
	int major, minor;

	if (read_block_dev_entry(path, &major, &minor))
		return 1;

	if (majdev == major && mindev == minor)
		return 0;

	return 1;
}

/* [한국어] /sys/block/ 하위에서 특정 major/minor를 가진 블록 디바이스 디렉토리를 재귀 탐색
 * 일치하는 dev 파일을 찾으면 해당 경로를 path에 저장하고 1(found)을 반환한다. */
static int find_block_dir(int majdev, int mindev, char *path, int link_ok)
{
	struct dirent *dir;
	struct stat st;
	int found = 0;
	DIR *D;

	D = opendir(path);
	if (!D)
		return 0;

	while ((dir = readdir(D)) != NULL) {
		char full_path[257];

		if (!strcmp(dir->d_name, ".") || !strcmp(dir->d_name, ".."))
			continue;

		sprintf(full_path, "%s/%s", path, dir->d_name);

		if (!strcmp(dir->d_name, "dev")) {
			if (!check_dev_match(majdev, mindev, full_path)) {
				found = 1;
				break;
			}
		}

		if (link_ok) {
			if (stat(full_path, &st) == -1) {
				perror("stat");
				break;
			}
		} else {
			if (lstat(full_path, &st) == -1) {
				perror("stat");
				break;
			}
		}

		if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode))
			continue;

		found = find_block_dir(majdev, mindev, full_path, 0);
		if (found) {
			strcpy(path, full_path);
			break;
		}
	}

	closedir(D);
	return found;
}

/* [한국어] sysfs 경로에서 파티션/디스크를 구분하여 올바른 디스크 루트 경로를 결정
 * ../queue/ 디렉토리가 존재하면 파티션 내에 있다는 뜻이므로 상위 디렉토리로 이동한다. */
static struct disk_util *__init_per_file_disk_util(struct thread_data *td,
						   int majdev, int mindev,
						   char *path)
{
	struct stat st;
	char tmp[PATH_MAX];
	char *p;

	/*
	 * If there's a ../queue/ directory there, we are inside a partition.
	 * Check if that is the case and jump back. For loop/md/dm etc we
	 * are already in the right spot.
	 */
	sprintf(tmp, "%s/../queue", path);
	if (!stat(tmp, &st)) {
		p = dirname(path);
		sprintf(tmp, "%s/queue", p);
		if (stat(tmp, &st)) {
			log_err("unknown sysfs layout\n");
			return NULL;
		}
		snprintf(tmp, FIO_ARRAY_SIZE(tmp), "%s", p);
		sprintf(path, "%s", tmp);
	}

	return disk_util_add(td, majdev, mindev, path);
}

/* [한국어] 파일별 디스크 유틸리티 초기화 - 파일 이름에서 디바이스를 찾아 등록
 * 캐시된 마지막 디바이스와 동일하면 sysfs 탐색을 건너뛴다. */
static struct disk_util *init_per_file_disk_util(struct thread_data *td,
						 char *filename)
{

	char foo[PATH_MAX];
	struct disk_util *du;
	int mindev, majdev;

	if (get_device_numbers(filename, &majdev, &mindev))
		return NULL;

	dprint(FD_DISKUTIL, "%s belongs to maj/min %d/%d\n", filename, majdev,
			mindev);

	/* 이미 등록된 디바이스인지 확인 */
	du = disk_util_exists(majdev, mindev);
	if (du)
		return du;

	/*
	 * for an fs without a device, we will repeatedly stat through
	 * sysfs which can take oodles of time for thousands of files. so
	 * cache the last lookup and compare with that before going through
	 * everything again.
	 */
	/* 캐시된 마지막 디바이스와 동일하면 sysfs 재탐색 생략 */
	if (mindev == last_mindev && majdev == last_majdev)
		return last_du;

	last_mindev = mindev;
	last_majdev = majdev;

	/* /sys/block/ 하위에서 해당 major/minor를 가진 디바이스 탐색 */
	sprintf(foo, "/sys/block");
	if (!find_block_dir(majdev, mindev, foo, 1))
		return NULL;

	return __init_per_file_disk_util(td, majdev, mindev, foo);
}

/* [한국어] 단일 파일에 대한 디스크 유틸리티 초기화 래퍼 함수 */
static struct disk_util *__init_disk_util(struct thread_data *td,
					  struct fio_file *f)
{
	return init_per_file_disk_util(td, f->file_name);
}

/* [한국어] 스레드의 모든 파일에 대해 디스크 유틸리티 초기화
 * do_disk_util 옵션이 활성화되어 있고, 디스크리스 I/O 엔진이 아닌 경우에만 수행한다. */
void init_disk_util(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;

	if (!td->o.do_disk_util ||
	    td_ioengine_flagged(td, FIO_DISKLESSIO | FIO_NODISKUTIL))
		return;

	for_each_file(td, f, i)
		f->du = __init_disk_util(td, f);
}

/* [한국어] 모든 디스크 유틸리티 항목 제거 및 메모리 해제
 * fio 종료 시 호출되어 disk_list의 모든 항목을 해제하고 세마포어를 제거한다. */
void disk_util_prune_entries(void)
{
	fio_sem_down(disk_util_sem);

	while (!flist_empty(&disk_list)) {
		struct disk_util *du;

		du = flist_first_entry(&disk_list, struct disk_util, list);
		flist_del(&du->list);
		disk_util_free(du);
	}

	last_majdev = last_mindev = -1;
	fio_sem_up(disk_util_sem);
	fio_sem_remove(disk_util_sem);
}

/* [한국어] 디스크 유틸리티 전역 초기화 - 세마포어를 생성하여 disk_list 동시 접근 보호 */
void setup_disk_util(void)
{
	disk_util_sem = fio_sem_init(FIO_SEM_UNLOCKED);
}
