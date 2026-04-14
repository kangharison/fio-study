/*
 * [한국어 설명] 레드-블랙 트리 헤더 (rbtree.h)
 *
 * === 파일의 역할 ===
 * Linux 커널 유래의 레드-블랙 트리 자료구조의 노드/루트 구조체, 매크로, API를 정의한다.
 * 사용자는 자체 삽입/검색 로직을 구현하고, rb_insert_color()로 균형을 유지한다.
 *
 * === 주요 알고리즘/자료구조 ===
 * - struct fio_rb_node: rb_parent_color(부모 포인터 + 색상 비트), rb_left, rb_right
 * - struct rb_root: 트리의 루트 노드 포인터
 * - rb_parent/rb_color 매크로: 포인터 하위 2비트를 이용한 색상 인코딩/디코딩
 * - rb_link_node: 새 노드를 트리에 연결 (색상 조정 전)
 * - rb_entry/container_of: 노드 포인터로부터 포함 구조체를 역참조
 * - RB_EMPTY_NODE/RB_CLEAR_NODE: 센티널 처리 매크로
 *
 * === fio에서의 사용 ===
 * fio의 다양한 정렬 자료구조에 사용된다. stat.c의 I/O 로그 관리,
 * smalloc의 메모리 풀 관리, 각종 타이머/이벤트 큐 등에서 rb_root를 선언하고
 * rb_insert_color/rb_erase로 노드를 관리한다.
 */

/*
  Red Black Trees
  (C) 1999  Andrea Arcangeli <andrea@suse.de>
  
  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.

  linux/include/linux/rbtree.h

  To use rbtrees you'll have to implement your own insert and search cores.
  This will avoid us to use callbacks and to drop drammatically performances.
  I know it's not the cleaner way,  but in C (not in C++) to get
  performances and genericity...

  Some example of insert and search follows here. The search is a plain
  normal search over an ordered tree. The insert instead must be implemented
  int two steps: as first thing the code must insert the element in
  order as a red leaf in the tree, then the support library function
  rb_insert_color() must be called. Such function will do the
  not trivial work to rebalance the rbtree if necessary.

-----------------------------------------------------------------------
static inline struct page * rb_search_page_cache(struct inode * inode,
						 unsigned long offset)
{
	struct fio_rb_node * n = inode->i_rb_page_cache.rb_node;
	struct page * page;

	while (n)
	{
		page = rb_entry(n, struct page, rb_page_cache);

		if (offset < page->offset)
			n = n->rb_left;
		else if (offset > page->offset)
			n = n->rb_right;
		else
			return page;
	}
	return NULL;
}

static inline struct page * __rb_insert_page_cache(struct inode * inode,
						   unsigned long offset,
						   struct fio_rb_node * node)
{
	struct fio_rb_node ** p = &inode->i_rb_page_cache.rb_node;
	struct fio_rb_node * parent = NULL;
	struct page * page;

	while (*p)
	{
		parent = *p;
		page = rb_entry(parent, struct page, rb_page_cache);

		if (offset < page->offset)
			p = &(*p)->rb_left;
		else if (offset > page->offset)
			p = &(*p)->rb_right;
		else
			return page;
	}

	rb_link_node(node, parent, p);

	return NULL;
}

static inline struct page * rb_insert_page_cache(struct inode * inode,
						 unsigned long offset,
						 struct fio_rb_node * node)
{
	struct page * ret;
	if ((ret = __rb_insert_page_cache(inode, offset, node)))
		goto out;
	rb_insert_color(node, &inode->i_rb_page_cache);
 out:
	return ret;
}
-----------------------------------------------------------------------
*/

#ifndef	_LINUX_RBTREE_H
#define	_LINUX_RBTREE_H

#include <stdlib.h>
#include <inttypes.h>

struct fio_rb_node
{
	intptr_t rb_parent_color;
	/* [한국어] 부모 포인터와 색상 비트를 하나의 필드에 인코딩.
	 * 하위 2비트: 색상 (0=RED, 1=BLACK), 나머지: 부모 노드 포인터.
	 * sizeof(long) 정렬로 하위 비트가 항상 0이므로 색상 저장에 활용 가능 */
#define	RB_RED		0
#define	RB_BLACK	1
	struct fio_rb_node *rb_right;	/* [한국어] 오른쪽 자식 (키가 큰 쪽) */
	struct fio_rb_node *rb_left;	/* [한국어] 왼쪽 자식 (키가 작은 쪽) */
} __attribute__((aligned(sizeof(long))));
    /* The alignment might seem pointless, but allegedly CRIS needs it */

struct rb_root
{
	struct fio_rb_node *rb_node;
	/* [한국어] 트리의 루트 노드 포인터. NULL이면 빈 트리 */
};


/* [한국어] rb_parent_color 필드에서 부모 포인터와 색상을 추출/설정하는 매크로 */
#define rb_parent(r)   ((struct fio_rb_node *)((r)->rb_parent_color & ~3))
#define rb_color(r)   ((r)->rb_parent_color & 1)
#define rb_is_red(r)   (!rb_color(r))
#define rb_is_black(r) rb_color(r)
#define rb_set_red(r)  do { (r)->rb_parent_color &= ~1; } while (0)
#define rb_set_black(r)  do { (r)->rb_parent_color |= 1; } while (0)

static inline void rb_set_parent(struct fio_rb_node *rb, struct fio_rb_node *p)
{
	rb->rb_parent_color = (rb->rb_parent_color & 3) | (uintptr_t)p;
}
static inline void rb_set_color(struct fio_rb_node *rb, int color)
{
	rb->rb_parent_color = (rb->rb_parent_color & ~1) | color;
}

#define RB_ROOT	(struct rb_root) { NULL, }
#define	rb_entry(ptr, type, member) container_of(ptr, type, member)

#define RB_EMPTY_ROOT(root)	((root)->rb_node == NULL)
#define RB_EMPTY_NODE(node)	(rb_parent(node) == node)
#define RB_CLEAR_NODE(node)	(rb_set_parent(node, node))

extern void rb_insert_color(struct fio_rb_node *, struct rb_root *);
extern void rb_erase(struct fio_rb_node *, struct rb_root *);

/* Find logical next and previous nodes in a tree */
extern struct fio_rb_node *rb_first(struct rb_root *);
extern struct fio_rb_node *rb_next(const struct fio_rb_node *);

/*
 * [한국어] rb_link_node - 새 노드를 트리에 연결 (색상 조정 전)
 *
 * @node: 삽입할 새 노드
 * @parent: 새 노드의 부모가 될 노드
 * @rb_link: 부모의 left 또는 right 포인터의 주소
 *
 * 이 함수 호출 후 반드시 rb_insert_color()를 호출해야 트리 균형이 유지된다.
 */
static inline void rb_link_node(struct fio_rb_node * node,
				struct fio_rb_node * parent,
				struct fio_rb_node ** rb_link)
{
	node->rb_parent_color = (uintptr_t)parent;
	node->rb_left = node->rb_right = NULL;

	*rb_link = node;
}

#endif	/* _LINUX_RBTREE_H */
