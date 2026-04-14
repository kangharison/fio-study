/*
 * [한국어 설명] 우선순위 검색 트리 헤더 (prio_tree.h)
 *
 * === 파일의 역할 ===
 * 우선순위 검색 트리(PST)의 노드/루트/이터레이터 구조체, 초기화 매크로,
 * 그리고 삽입/삭제/순회 API를 정의한다.
 *
 * === 주요 알고리즘/자료구조 ===
 * - struct prio_tree_node: left, right, parent 포인터와 start/last(구간 범위)
 * - struct prio_tree_root: 루트 노드 포인터와 index_bits(트리 높이 결정)
 * - struct prio_tree_iter: 순회 상태 (cur, mask, value, size_level, 쿼리 범위)
 * - INIT_PRIO_TREE_NODE: 노드의 left/right/parent를 자기 자신으로 초기화 (센티널)
 * - prio_tree_empty/prio_tree_root/prio_tree_left_empty/prio_tree_right_empty:
 *   센티널 기반 상태 확인 인라인 함수들
 * - prio_tree_entry: 노드 포인터로부터 포함 구조체를 역참조하는 매크로
 *
 * === fio에서의 사용 ===
 * gfio의 graph.c에서 그래프 데이터 포인트의 범위 쿼리에 사용된다.
 * prio_tree_iter_init으로 쿼리 범위를 설정하고, prio_tree_next로
 * 겹치는 구간을 순회하여 해당 데이터 포인트의 툴팁 정보를 표시한다.
 */

#ifndef _LINUX_PRIO_TREE_H
#define _LINUX_PRIO_TREE_H

#include <inttypes.h>

struct prio_tree_node {
	struct prio_tree_node	*left;		/* [한국어] 왼쪽 자식 (자기 자신이면 비어 있음) */
	struct prio_tree_node	*right;		/* [한국어] 오른쪽 자식 */
	struct prio_tree_node	*parent;	/* [한국어] 부모 (자기 자신이면 루트) */
	uint64_t		start;		/* [한국어] 구간의 시작 (radix_index) */
	uint64_t		last;		/* [한국어] 구간의 끝 (heap_index, start <= last) */
};

struct prio_tree_root {
	struct prio_tree_node	*prio_tree_node;	/* [한국어] 트리의 루트 노드 (NULL이면 빈 트리) */
	unsigned short 		index_bits;	/* [한국어] 트리 높이 결정. max_heap_index < 2^index_bits */
};

struct prio_tree_iter {
	struct prio_tree_node	*cur;		/* [한국어] 현재 순회 위치 */
	unsigned long		mask;		/* [한국어] 현재 레벨의 비트 마스크 */
	unsigned long		value;		/* [한국어] 지금까지 누적된 인덱스 값 */
	int			size_level;	/* [한국어] 0=radix 인덱스 단계, >0=size 인덱스 단계 */

	struct prio_tree_root	*root;		/* [한국어] 검색 대상 트리 루트 */
	uint64_t		r_index;	/* [한국어] 쿼리 구간의 시작 (radix_index) */
	uint64_t		h_index;	/* [한국어] 쿼리 구간의 끝 (heap_index) */
};

static inline void prio_tree_iter_init(struct prio_tree_iter *iter,
		struct prio_tree_root *root, uint64_t r_index, uint64_t h_index)
{
	iter->root = root;
	iter->r_index = r_index;
	iter->h_index = h_index;
	iter->cur = NULL;
}

#define INIT_PRIO_TREE_ROOT(ptr)	\
do {					\
	(ptr)->prio_tree_node = NULL;	\
	(ptr)->index_bits = 1;		\
} while (0)

#define INIT_PRIO_TREE_NODE(ptr)				\
do {								\
	(ptr)->left = (ptr)->right = (ptr)->parent = (ptr);	\
} while (0)

#define INIT_PRIO_TREE_ITER(ptr)	\
do {					\
	(ptr)->cur = NULL;		\
	(ptr)->mask = 0UL;		\
	(ptr)->value = 0UL;		\
	(ptr)->size_level = 0;		\
} while (0)

#define prio_tree_entry(ptr, type, member) \
       ((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))

static inline int prio_tree_empty(const struct prio_tree_root *root)
{
	return root->prio_tree_node == NULL;
}

static inline int prio_tree_root(const struct prio_tree_node *node)
{
	return node->parent == node;
}

static inline int prio_tree_left_empty(const struct prio_tree_node *node)
{
	return node->left == node;
}

static inline int prio_tree_right_empty(const struct prio_tree_node *node)
{
	return node->right == node;
}


struct prio_tree_node *prio_tree_replace(struct prio_tree_root *root,
                struct prio_tree_node *old, struct prio_tree_node *node);
struct prio_tree_node *prio_tree_insert(struct prio_tree_root *root,
                struct prio_tree_node *node);
void prio_tree_remove(struct prio_tree_root *root, struct prio_tree_node *node);
struct prio_tree_node *prio_tree_next(struct prio_tree_iter *iter);

#endif /* _LINUX_PRIO_TREE_H */
