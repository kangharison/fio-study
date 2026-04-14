/*
 * [한국어] filelock.c - 파일명 기반 배타적 잠금 구현
 *
 * 파일명의 해시값을 키로 사용하여 동일 파일에 대한 동시 접근을 동기화한다.
 * 해시 인덱싱 없이 단순 연결 리스트를 사용하므로 100개 미만의 파일에 적합하다.
 *
 * 구조:
 *   filelock_data (전역) - 활성 잠금 리스트 + 프리 리스트 + 전역 뮤텍스
 *   fio_filelock        - 개별 파일 잠금 (해시값 + 세마포어 + 참조 카운트)
 *
 * 주요 API:
 *   fio_filelock_init()  - 잠금 시스템 초기화 (MAX_FILELOCKS개 사전 할당)
 *   fio_lock_file()      - 파일 잠금 획득 (차단)
 *   fio_trylock_file()   - 파일 잠금 시도 (비차단)
 *   fio_unlock_file()    - 파일 잠금 해제
 *   fio_filelock_exit()  - 잠금 시스템 종료 및 자원 해제
 
 * === 파일의 역할 ===
 * 파일명의 해시값을 키로 사용하여 동일 파일에 대한 동시 접근을 동기화.
 *
 * === 전체 아키텍처에서의 위치 ===
 * libfio.c에서 fio_filelock_init()으로 초기화. filesetup.c에서 잠금 사용.
 *
 * === 타 모듈과의 연결 ===
 * - libfio.c: 초기화/정리
 * - filesetup.c: 파일 열기/닫기 시 잠금
 * - filelock.h: API 선언
 *
 * === 주요 함수/구조체 요약 ===
 * - fio_lock_file(): 파일 잠금 획득 (차단)
 * - fio_unlock_file(): 파일 잠금 해제
 */

/*
 * Really simple exclusive file locking based on filename.
 * No hash indexing, just a list, so only works well for < 100 files or
 * so. But that's more than what fio needs, so should be fine.
 */
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include "flist.h"
#include "filelock.h"
#include "smalloc.h"
#include "fio_sem.h"
#include "hash.h"
#include "log.h"

/* [한국어] 개별 파일 잠금 구조체 */
struct fio_filelock {
	uint32_t hash;			/* [한국어] 파일명의 jhash 값 (검색 키) */
	struct fio_sem lock;		/* [한국어] 해당 파일에 대한 세마포어 (배타적 잠금) */
	struct flist_head list;		/* [한국어] 활성 리스트 또는 프리 리스트에 연결 */
	unsigned int references;	/* [한국어] 이 잠금을 참조하는 스레드 수 */
};

/* [한국어] 사전 할당되는 최대 파일 잠금 수 */
#define MAX_FILELOCKS	1024

/* [한국어] 파일 잠금 시스템의 전역 데이터 구조체 */
static struct filelock_data {
	struct flist_head list;		/* [한국어] 활성 잠금 리스트 (사용 중인 파일 잠금들) */
	struct fio_sem lock;		/* [한국어] 전역 보호 뮤텍스 (리스트 접근 동기화) */

	struct flist_head free_list;	/* [한국어] 프리 리스트 (미사용 잠금 객체들) */
	struct fio_filelock ffs[MAX_FILELOCKS];	/* [한국어] 사전 할당된 잠금 객체 배열 */
} *fld;

/* [한국어] 잠금 객체를 프리 리스트로 반환 */
static void put_filelock(struct fio_filelock *ff)
{
	flist_add(&ff->list, &fld->free_list);
}

/* [한국어] 프리 리스트에서 잠금 객체를 꺼냄 (없으면 NULL 반환) */
static struct fio_filelock *__get_filelock(void)
{
	struct fio_filelock *ff;

	if (flist_empty(&fld->free_list))
		return NULL;

	ff = flist_first_entry(&fld->free_list, struct fio_filelock, list);
	flist_del_init(&ff->list);
	return ff;
}

/* [한국어] 잠금 객체 획득. trylock이 아니면 사용 가능해질 때까지 재시도 */
static struct fio_filelock *get_filelock(int trylock, int *retry)
{
	struct fio_filelock *ff;

	do {
		ff = __get_filelock();
		if (ff || trylock)
			break;

		/* [한국어] 프리 리스트가 비어있으면 전역 락을 풀고 잠시 대기 후 재시도 */
		fio_sem_up(&fld->lock);
		usleep(1000);
		fio_sem_down(&fld->lock);
		*retry = 1;	/* [한국어] 재시도 플래그 설정 (해시 재검색 필요) */
	} while (1);

	return ff;
}

/* [한국어] 파일 잠금 시스템 초기화: 공유 메모리에 전역 데이터 할당 및 초기화 */
int fio_filelock_init(void)
{
	int i;

	/* [한국어] smalloc으로 공유 메모리에 전역 데이터 할당 */
	fld = smalloc(sizeof(*fld));
	if (!fld)
		return 1;

	INIT_FLIST_HEAD(&fld->list);
	INIT_FLIST_HEAD(&fld->free_list);

	/* [한국어] 전역 보호 뮤텍스를 잠금 해제 상태로 초기화 */
	if (__fio_sem_init(&fld->lock, FIO_SEM_UNLOCKED))
		goto err;

	/* [한국어] MAX_FILELOCKS개의 잠금 객체를 사전 초기화하여 프리 리스트에 추가 */
	for (i = 0; i < MAX_FILELOCKS; i++) {
		struct fio_filelock *ff = &fld->ffs[i];

		if (__fio_sem_init(&ff->lock, FIO_SEM_UNLOCKED))
			goto err;
		flist_add_tail(&ff->list, &fld->free_list);
	}

	return 0;
err:
	fio_filelock_exit();
	return 1;
}

/* [한국어] 파일 잠금 시스템 종료: 모든 세마포어 파괴 및 공유 메모리 해제 */
void fio_filelock_exit(void)
{
	if (!fld)
		return;

	/* [한국어] 활성 잠금이 남아있으면 안 됨 (버그 검출) */
	assert(flist_empty(&fld->list));
	__fio_sem_remove(&fld->lock);

	/* [한국어] 프리 리스트의 모든 세마포어를 파괴 */
	while (!flist_empty(&fld->free_list)) {
		struct fio_filelock *ff;

		ff = flist_first_entry(&fld->free_list, struct fio_filelock, list);

		flist_del_init(&ff->list);
		__fio_sem_remove(&ff->lock);
	}

	sfree(fld);
	fld = NULL;
}

/* [한국어] 해시값으로 활성 잠금 리스트에서 파일 잠금을 검색 */
static struct fio_filelock *fio_hash_find(uint32_t hash)
{
	struct flist_head *entry;
	struct fio_filelock *ff;

	flist_for_each(entry, &fld->list) {
		ff = flist_entry(entry, struct fio_filelock, list);
		if (ff->hash == hash)
			return ff;
	}

	return NULL;
}

/* [한국어] 해시값에 해당하는 잠금 검색, 없으면 새로 생성하여 활성 리스트에 추가 */
static struct fio_filelock *fio_hash_get(uint32_t hash, int trylock)
{
	struct fio_filelock *ff;

	ff = fio_hash_find(hash);
	if (!ff) {
		int retry = 0;

		ff = get_filelock(trylock, &retry);
		if (!ff)
			return NULL;

		/*
		 * If we dropped the main lock, re-lookup the hash in case
		 * someone else added it meanwhile. If it's now there,
		 * just return that.
		 */
		/* [한국어] 전역 락을 풀었다 다시 잡은 경우, 다른 스레드가 같은 해시를 추가했을 수 있으므로 재검색 */
		if (retry) {
			struct fio_filelock *__ff;

			__ff = fio_hash_find(hash);
			if (__ff) {
				put_filelock(ff);
				return __ff;
			}
		}

		ff->hash = hash;
		ff->references = 0;
		flist_add(&ff->list, &fld->list);	/* [한국어] 활성 리스트에 추가 */
	}

	return ff;
}

/* [한국어] 파일 잠금 획득의 내부 구현 (차단/비차단 공통) */
static bool __fio_lock_file(const char *fname, int trylock)
{
	struct fio_filelock *ff;
	uint32_t hash;

	/* [한국어] 파일명을 jhash로 해시값 계산 */
	hash = jhash(fname, strlen(fname), 0);

	/* [한국어] 전역 락 보호 하에 해시 조회 및 참조 카운트 증가 */
	fio_sem_down(&fld->lock);
	ff = fio_hash_get(hash, trylock);
	if (ff)
		ff->references++;
	fio_sem_up(&fld->lock);

	if (!ff) {
		assert(trylock);
		return true;	/* [한국어] trylock 모드에서 잠금 객체 할당 실패 */
	}

	/* [한국어] 차단 모드: 파일별 세마포어를 획득할 때까지 대기 */
	if (!trylock) {
		fio_sem_down(&ff->lock);
		return false;
	}

	/* [한국어] 비차단 모드: trylock 시도 */
	if (!fio_sem_down_trylock(&ff->lock))
		return false;	/* [한국어] 잠금 획득 성공 */

	/* [한국어] trylock 실패: 참조 카운트를 줄이고, 유일한 참조자면 강제 획득 */
	fio_sem_down(&fld->lock);

	/*
	 * If we raced and the only reference to the lock is us, we can
	 * grab it
	 */
	if (ff->references != 1) {
		ff->references--;
		ff = NULL;
	}

	fio_sem_up(&fld->lock);

	if (ff) {
		fio_sem_down(&ff->lock);	/* [한국어] 유일한 참조자이므로 잠금 획득 */
		return false;
	}

	return true;	/* [한국어] 잠금 획득 실패 */
}

/* [한국어] 파일 잠금 비차단 시도 (잠금 실패 시 true 반환) */
bool fio_trylock_file(const char *fname)
{
	return __fio_lock_file(fname, 1);
}

/* [한국어] 파일 잠금 획득 (잠금될 때까지 차단) */
void fio_lock_file(const char *fname)
{
	__fio_lock_file(fname, 0);
}

/* [한국어] 파일 잠금 해제: 참조 카운트 감소 후 0이면 프리 리스트로 반환 */
void fio_unlock_file(const char *fname)
{
	struct fio_filelock *ff;
	uint32_t hash;

	hash = jhash(fname, strlen(fname), 0);

	fio_sem_down(&fld->lock);

	ff = fio_hash_find(hash);
	if (ff) {
		int refs = --ff->references;
		fio_sem_up(&ff->lock);	/* [한국어] 파일별 세마포어 해제 (V 연산) */
		if (!refs) {
			/* [한국어] 참조자가 없으면 활성 리스트에서 제거하고 프리 리스트로 반환 */
			flist_del_init(&ff->list);
			put_filelock(ff);
		}
	} else
		log_err("fio: file not found for unlocking\n");

	fio_sem_up(&fld->lock);
}
