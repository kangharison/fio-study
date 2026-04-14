/*
 * [한국어 설명] fio 연결 리스트 병합 정렬 (flist_sort.c)
 *
 * === 파일의 역할 ===
 * fio의 이중 연결 리스트(flist)를 위한 병합 정렬(merge sort) 구현이다.
 * Linux 커널의 list_sort에서 가져온 코드로, O(n log n) 시간 복잡도를 가지며
 * 안정 정렬(stable sort)을 보장한다.
 *
 * === fio에서의 사용 ===
 * fio 내부에서 연결 리스트로 관리되는 데이터(예: I/O 큐, 통계 항목 등)를
 * 비교 함수 기반으로 정렬해야 할 때 flist_sort()를 호출한다.
 */
#include <stdio.h>
#include <string.h>
#include "../flist.h"
#include "../log.h"

#define MAX_LIST_LENGTH_BITS 20

/*
 * Returns a list organized in an intermediate format suited
 * to chaining of merge() calls: null-terminated, no reserved or
 * sentinel head node, "prev" links not maintained.
 */
/*
 * [한국어] merge - 두 정렬된 리스트를 하나로 병합
 *
 * @priv: 비교 함수에 전달되는 사용자 데이터
 * @cmp: 비교 함수 (음수: a가 먼저, 양수: b가 먼저, 0: 동일)
 * @a, @b: 병합할 두 리스트의 첫 노드
 * @return: 병합된 리스트의 첫 노드 (null-terminated, prev 미유지)
 *
 * 동일한 키의 원소는 a에서 먼저 가져와 안정 정렬(stable sort)을 보장한다.
 */
static struct flist_head *merge(void *priv,
				int (*cmp)(void *priv, struct flist_head *a,
					struct flist_head *b),
				struct flist_head *a, struct flist_head *b)
{
	struct flist_head head, *tail = &head;

	while (a && b) {
		/* if equal, take 'a' -- important for sort stability */
		if ((*cmp)(priv, a, b) <= 0) {
			tail->next = a;
			a = a->next;
		} else {
			tail->next = b;
			b = b->next;
		}
		tail = tail->next;
	}
	tail->next = a?:b;
	return head.next;
}

/*
 * Combine final list merge with restoration of standard doubly-linked
 * list structure.  This approach duplicates code from merge(), but
 * runs faster than the tidier alternatives of either a separate final
 * prev-link restoration pass, or maintaining the prev links
 * throughout.
 */
/*
 * [한국어] merge_and_restore_back_links - 최종 병합과 이중 연결 리스트 prev 링크 복원
 *
 * 마지막 병합 단계에서 prev 링크를 동시에 설정하여 별도의 복원 패스를 생략한다.
 * head->prev = 마지막 노드, 마지막 노드->next = head로 원형 리스트를 완성한다.
 */
static void merge_and_restore_back_links(void *priv,
				int (*cmp)(void *priv, struct flist_head *a,
					struct flist_head *b),
				struct flist_head *head,
				struct flist_head *a, struct flist_head *b)
{
	struct flist_head *tail = head;

	while (a && b) {
		/* if equal, take 'a' -- important for sort stability */
		if ((*cmp)(priv, a, b) <= 0) {
			tail->next = a;
			a->prev = tail;
			a = a->next;
		} else {
			tail->next = b;
			b->prev = tail;
			b = b->next;
		}
		tail = tail->next;
	}
	tail->next = a ? : b;

	do {
		/*
		 * In worst cases this loop may run many iterations.
		 * Continue callbacks to the client even though no
		 * element comparison is needed, so the client's cmp()
		 * routine can invoke cond_resched() periodically.
		 */
		(*cmp)(priv, tail->next, tail->next);

		tail->next->prev = tail;
		tail = tail->next;
	} while (tail->next);

	tail->next = head;
	head->prev = tail;
}

/**
 * list_sort - sort a list
 * @priv: private data, opaque to list_sort(), passed to @cmp
 * @head: the list to sort
 * @cmp: the elements comparison function
 *
 * This function implements "merge sort", which has O(nlog(n))
 * complexity.
 *
 * The comparison function @cmp must return a negative value if @a
 * should sort before @b, and a positive value if @a should sort after
 * @b. If @a and @b are equivalent, and their original relative
 * ordering is to be preserved, @cmp must return 0.
 */
/*
 * [한국어] flist_sort - fio의 이중 연결 리스트를 O(n log n) 병합 정렬
 *
 * @priv: 비교 함수에 전달되는 사용자 데이터
 * @head: 정렬할 리스트의 헤드
 * @cmp: 비교 함수
 *
 * 바텀업 병합 정렬을 사용한다. part[i]에는 크기 2^i의 정렬된 부분 리스트가 저장되며,
 * 새 원소가 올 때마다 같은 크기의 부분 리스트와 병합하여 상위로 올린다.
 * 최종적으로 모든 부분 리스트를 병합하고 이중 연결 구조를 복원한다.
 *
 * 호출 체인: fio 내부 코드 → [flist_sort] → merge → merge_and_restore_back_links
 */
void flist_sort(void *priv, struct flist_head *head,
		int (*cmp)(void *priv, struct flist_head *a,
			struct flist_head *b))
{
	/* [한국어] part[i]: 크기 2^i의 정렬된 부분 리스트. 마지막 슬롯은 센티널 */
	struct flist_head *part[MAX_LIST_LENGTH_BITS+1]; /* sorted partial lists
						-- last slot is a sentinel */
	int lev;  /* index into part[] */
	int max_lev = 0;
	struct flist_head *list;

	if (flist_empty(head))
		return;

	memset(part, 0, sizeof(part));

	head->prev->next = NULL;
	list = head->next;

	while (list) {
		struct flist_head *cur = list;
		list = list->next;
		cur->next = NULL;

		for (lev = 0; part[lev]; lev++) {
			cur = merge(priv, cmp, part[lev], cur);
			part[lev] = NULL;
		}
		if (lev > max_lev) {
			if (lev >= MAX_LIST_LENGTH_BITS) {
				log_err("fio: list passed to"
					" list_sort() too long for"
					" efficiency\n");
				lev--;
			}
			max_lev = lev;
		}
		part[lev] = cur;
	}

	for (lev = 0; lev < max_lev; lev++)
		if (part[lev])
			list = merge(priv, cmp, part[lev], list);

	merge_and_restore_back_links(priv, cmp, head, part[max_lev], list);
}
