/*
 * [한국어] filehash.c - 파일 해시 테이블 구현 (동일 파일 공유 관리)
 *
 * 여러 fio job이 동일한 파일에 접근할 때, 파일명 기반 해시 테이블을 통해
 * fio_file 구조체를 공유 관리한다. bloom 필터로 빠른 존재 여부 확인도 지원한다.
 *
 * 구조:
 *   file_hash[]  - 512개 버킷의 해시 테이블 (flist_head 배열)
 *   hash_lock    - 해시 테이블 전역 보호 세마포어
 *   file_bloom   - 16MB bloom 필터 (빠른 존재 확인)
 *
 * 주요 API:
 *   file_hash_init()     - 해시 테이블/bloom 필터 초기화
 *   add_file_hash()      - 파일을 해시 테이블에 추가 (중복 시 alias 반환)
 *   lookup_file_hash()   - 파일명으로 검색
 *   remove_file_hash()   - 해시 테이블에서 파일 제거
 *   file_bloom_exists()  - bloom 필터로 존재 여부 확인
 *   file_hash_exit()     - 자원 해제
 */
#include <stdlib.h>
#include <assert.h>

#include "fio.h"
#include "flist.h"
#include "hash.h"
#include "filehash.h"
#include "smalloc.h"
#include "lib/bloom.h"

/* [한국어] 해시 테이블 설정: 512개 버킷, 비트마스크 0x1FF */
#define HASH_BUCKETS	512
#define HASH_MASK	(HASH_BUCKETS - 1)

/* [한국어] bloom 필터 크기: 16MB */
#define BLOOM_SIZE	16*1024*1024

/* [한국어] 해시 테이블 전체 크기 (바이트) */
static unsigned int file_hash_size = HASH_BUCKETS * sizeof(struct flist_head);

static struct flist_head *file_hash;	/* [한국어] 해시 버킷 배열 (smalloc으로 공유 메모리에 할당) */
static struct fio_sem *hash_lock;	/* [한국어] 해시 테이블 전역 보호 세마포어 */
static struct bloom *file_bloom;	/* [한국어] 파일 존재 여부 빠른 확인용 bloom 필터 */

/* [한국어] 파일명을 jhash로 해시하여 버킷 인덱스 계산 */
static unsigned short hash(const char *name)
{
	return jhash(name, strlen(name), 0) & HASH_MASK;
}

/* [한국어] 해시 테이블 전역 세마포어 획득 (외부에서 직접 잠금 필요 시) */
void fio_file_hash_lock(void)
{
	if (hash_lock)
		fio_sem_down(hash_lock);
}

/* [한국어] 해시 테이블 전역 세마포어 해제 */
void fio_file_hash_unlock(void)
{
	if (hash_lock)
		fio_sem_up(hash_lock);
}

/* [한국어] 해시 테이블에서 fio_file을 제거하고 해시 플래그를 클리어 */
void remove_file_hash(struct fio_file *f)
{
	fio_sem_down(hash_lock);

	if (fio_file_hashed(f)) {
		assert(!flist_empty(&f->hash_list));
		flist_del_init(&f->hash_list);
		fio_file_clear_hashed(f);
	}

	fio_sem_up(hash_lock);
}

/* [한국어] 파일명으로 해시 버킷을 순회하여 fio_file 검색 (내부용, 락 없음) */
static struct fio_file *__lookup_file_hash(const char *name)
{
	struct flist_head *bucket = &file_hash[hash(name)];
	struct flist_head *n;

	flist_for_each(n, bucket) {
		struct fio_file *f = flist_entry(n, struct fio_file, hash_list);

		if (!f->file_name)
			continue;

		if (!strcmp(f->file_name, name))
			return f;
	}

	return NULL;
}

/* [한국어] 파일명으로 해시 테이블에서 fio_file 검색 (세마포어 보호) */
struct fio_file *lookup_file_hash(const char *name)
{
	struct fio_file *f;

	fio_sem_down(hash_lock);
	f = __lookup_file_hash(name);
	fio_sem_up(hash_lock);
	return f;
}

/* [한국어] fio_file을 해시 테이블에 추가. 동일 파일이 이미 있으면 alias(기존 항목) 반환 */
struct fio_file *add_file_hash(struct fio_file *f)
{
	struct fio_file *alias;

	/* [한국어] 이미 해시에 등록된 파일이면 추가하지 않음 */
	if (fio_file_hashed(f))
		return NULL;

	INIT_FLIST_HEAD(&f->hash_list);

	fio_sem_down(hash_lock);

	/* [한국어] 동일 파일명이 이미 존재하는지 확인 */
	alias = __lookup_file_hash(f->file_name);
	if (!alias) {
		/* [한국어] 새 파일: 해시 플래그 설정 후 버킷에 추가 */
		fio_file_set_hashed(f);
		flist_add_tail(&f->hash_list, &file_hash[hash(f->file_name)]);
	}

	fio_sem_up(hash_lock);
	return alias;	/* [한국어] NULL이면 새로 추가됨, 아니면 기존 파일 반환 */
}

/* [한국어] bloom 필터로 파일 존재 여부를 빠르게 확인 (set=true면 필터에 추가도 수행) */
bool file_bloom_exists(const char *fname, bool set)
{
	return bloom_string(file_bloom, fname, strlen(fname), set);
}

/* [한국어] 해시 테이블 종료: 잔여 항목 확인 후 모든 자원 해제 */
void file_hash_exit(void)
{
	unsigned int i, has_entries = 0;

	/* [한국어] 비어있지 않은 버킷 수를 확인 (정상 종료 시 0이어야 함) */
	fio_sem_down(hash_lock);
	for (i = 0; i < HASH_BUCKETS; i++)
		has_entries += !flist_empty(&file_hash[i]);
	fio_sem_up(hash_lock);

	if (has_entries)
		log_err("fio: file hash not empty on exit\n");

	sfree(file_hash);	/* [한국어] 공유 메모리의 해시 버킷 배열 해제 */
	file_hash = NULL;
	fio_sem_remove(hash_lock);	/* [한국어] 세마포어 파괴 및 mmap 해제 */
	hash_lock = NULL;
	bloom_free(file_bloom);		/* [한국어] bloom 필터 해제 */
	file_bloom = NULL;
}

/* [한국어] 해시 테이블 초기화: 버킷 배열 할당, 세마포어 생성, bloom 필터 생성 */
void file_hash_init(void)
{
	unsigned int i;

	/* [한국어] smalloc으로 공유 메모리에 해시 버킷 배열 할당 */
	file_hash = smalloc(file_hash_size);

	/* [한국어] 모든 버킷을 빈 리스트로 초기화 */
	for (i = 0; i < HASH_BUCKETS; i++)
		INIT_FLIST_HEAD(&file_hash[i]);

	/* [한국어] 해시 테이블 보호용 세마포어 생성 (잠금 해제 상태) */
	hash_lock = fio_sem_init(FIO_SEM_UNLOCKED);
	/* [한국어] 16MB bloom 필터 생성 */
	file_bloom = bloom_new(BLOOM_SIZE);
}
