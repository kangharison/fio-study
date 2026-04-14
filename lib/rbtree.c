/*
 * [한국어 설명] 레드-블랙 트리 구현 (rbtree.c)
 *
 * === 파일의 역할 ===
 * Linux 커널에서 가져온 레드-블랙 트리(자가 균형 이진 탐색 트리) 구현이다.
 * 삽입, 삭제, 순회 연산을 O(log n) 시간에 수행하며,
 * 색상 재조정과 회전 연산으로 트리의 균형을 유지한다.
 *
 * === 주요 알고리즘/자료구조 ===
 * - __rb_rotate_left/__rb_rotate_right: 좌/우 회전으로 트리 균형 조정
 * - rb_insert_color: 삽입 후 레드-블랙 속성 복원 (삼촌 노드 색상에 따른 케이스 처리)
 * - __rb_erase_color: 삭제 후 레드-블랙 속성 복원 (형제 노드 색상에 따른 케이스 처리)
 * - rb_erase: 노드 삭제 시 후계자(successor) 노드로 교체 후 색상 보정
 * - rb_first/rb_next: 정렬 순서의 최소값 조회 및 중위 순회(in-order traversal)
 * - rb_parent_color 필드: 포인터 하위 비트에 색상 정보를 인코딩하여 메모리 절약
 *
 * === fio에서의 사용 ===
 * fio 전체에서 정렬된 데이터 관리에 사용된다. I/O 로그의 시간순 정렬,
 * 대기 중인 I/O 요청의 우선순위 관리, iolog의 오프셋별 정렬 등에 활용된다.
 */

/*
  Red Black Trees
  (C) 1999  Andrea Arcangeli <andrea@suse.de>
  (C) 2002  David Woodhouse <dwmw2@infradead.org>
  
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

  linux/lib/rbtree.c
*/

#include "rbtree.h"

/*
 * [한국어] __rb_rotate_left - 노드를 기준으로 좌회전
 *
 * node의 오른쪽 자식(right)이 node의 자리를 대체하고,
 * node는 right의 왼쪽 자식이 된다. 트리 높이 균형을 조정하는 기본 연산.
 *
 *     node              right
 *    /    \     →      /     \
 *   T1   right       node    T3
 *        /  \        /   \
 *       T2   T3     T1    T2
 */
static void __rb_rotate_left(struct fio_rb_node *node, struct rb_root *root)
{
	struct fio_rb_node *right = node->rb_right;
	struct fio_rb_node *parent = rb_parent(node);

	if ((node->rb_right = right->rb_left))
		rb_set_parent(right->rb_left, node);
	right->rb_left = node;

	rb_set_parent(right, parent);

	if (parent)
	{
		if (node == parent->rb_left)
			parent->rb_left = right;
		else
			parent->rb_right = right;
	}
	else
		root->rb_node = right;
	rb_set_parent(node, right);
}

/*
 * [한국어] __rb_rotate_right - 노드를 기준으로 우회전 (좌회전의 대칭)
 */
static void __rb_rotate_right(struct fio_rb_node *node, struct rb_root *root)
{
	struct fio_rb_node *left = node->rb_left;
	struct fio_rb_node *parent = rb_parent(node);

	if ((node->rb_left = left->rb_right))
		rb_set_parent(left->rb_right, node);
	left->rb_right = node;

	rb_set_parent(left, parent);

	if (parent)
	{
		if (node == parent->rb_right)
			parent->rb_right = left;
		else
			parent->rb_left = left;
	}
	else
		root->rb_node = left;
	rb_set_parent(node, left);
}

/*
 * [한국어] rb_insert_color - 삽입 후 레드-블랙 트리 속성을 복원
 *
 * @node: 새로 삽입된 노드 (초기에 RED로 설정됨)
 * @root: 트리 루트
 *
 * 삽입된 RED 노드의 부모도 RED이면 속성 위반이므로,
 * 삼촌 노드의 색상에 따라 3가지 케이스를 처리한다:
 * - 케이스 1: 삼촌이 RED → 부모와 삼촌을 BLACK으로, 조부모를 RED로 변경 후 상향
 * - 케이스 2: 삼촌이 BLACK + 노드가 부모의 안쪽 자식 → 회전으로 케이스 3으로 변환
 * - 케이스 3: 삼촌이 BLACK + 노드가 부모의 바깥 자식 → 색상 변경 + 회전
 *
 * 호출 체인: 사용자 삽입 코드 → rb_link_node() → [rb_insert_color]
 */
void rb_insert_color(struct fio_rb_node *node, struct rb_root *root)
{
	struct fio_rb_node *parent, *gparent;

	/* [한국어] 부모가 RED인 동안 반복하여 속성 위반을 수정 */
	while ((parent = rb_parent(node)) && rb_is_red(parent))
	{
		gparent = rb_parent(parent);

		if (parent == gparent->rb_left)
		{
			{
				/* [한국어] 케이스 1: 삼촌이 RED → 색상만 변경하고 조부모로 상향 */
				register struct fio_rb_node *uncle = gparent->rb_right;
				if (uncle && rb_is_red(uncle))
				{
					rb_set_black(uncle);
					rb_set_black(parent);
					rb_set_red(gparent);
					node = gparent;
					continue;
				}
			}

			/* [한국어] 케이스 2: 안쪽 자식 → 좌회전으로 케이스 3으로 변환 */
			if (parent->rb_right == node)
			{
				register struct fio_rb_node *tmp;
				__rb_rotate_left(parent, root);
				tmp = parent;
				parent = node;
				node = tmp;
			}

			/* [한국어] 케이스 3: 바깥 자식 → 부모를 BLACK, 조부모를 RED로 변경 후 우회전 */
			rb_set_black(parent);
			rb_set_red(gparent);
			__rb_rotate_right(gparent, root);
		} else {
			{
				/* [한국어] 대칭 케이스: 부모가 조부모의 오른쪽 자식인 경우 */
				register struct fio_rb_node *uncle = gparent->rb_left;
				if (uncle && rb_is_red(uncle))
				{
					rb_set_black(uncle);
					rb_set_black(parent);
					rb_set_red(gparent);
					node = gparent;
					continue;
				}
			}

			if (parent->rb_left == node)
			{
				register struct fio_rb_node *tmp;
				__rb_rotate_right(parent, root);
				tmp = parent;
				parent = node;
				node = tmp;
			}

			rb_set_black(parent);
			rb_set_red(gparent);
			__rb_rotate_left(gparent, root);
		}
	}

	/* [한국어] 루트는 항상 BLACK이어야 함 (레드-블랙 트리 속성 2) */
	rb_set_black(root->rb_node);
}

/*
 * [한국어] __rb_erase_color - 삭제 후 레드-블랙 트리 속성을 복원
 *
 * @node: 삭제된 노드의 자식 (NULL일 수 있음)
 * @parent: node의 부모
 * @root: 트리 루트
 *
 * BLACK 노드가 삭제되면 "이중 흑색" 문제가 발생한다. 형제(sibling) 노드의
 * 색상과 형제 자식들의 색상에 따라 4가지 케이스를 처리하여 속성을 복원한다.
 */
static void __rb_erase_color(struct fio_rb_node *node,
			     struct fio_rb_node *parent,
			     struct rb_root *root)
{
	struct fio_rb_node *other;

	while ((!node || rb_is_black(node)) && node != root->rb_node)
	{
		if (parent->rb_left == node)
		{
			other = parent->rb_right;
			if (rb_is_red(other))
			{
				rb_set_black(other);
				rb_set_red(parent);
				__rb_rotate_left(parent, root);
				other = parent->rb_right;
			}
			if ((!other->rb_left || rb_is_black(other->rb_left)) &&
			    (!other->rb_right || rb_is_black(other->rb_right)))
			{
				rb_set_red(other);
				node = parent;
				parent = rb_parent(node);
			}
			else
			{
				if (!other->rb_right || rb_is_black(other->rb_right))
				{
					struct fio_rb_node *o_left;
					if ((o_left = other->rb_left))
						rb_set_black(o_left);
					rb_set_red(other);
					__rb_rotate_right(other, root);
					other = parent->rb_right;
				}
				rb_set_color(other, rb_color(parent));
				rb_set_black(parent);
				if (other->rb_right)
					rb_set_black(other->rb_right);
				__rb_rotate_left(parent, root);
				node = root->rb_node;
				break;
			}
		}
		else
		{
			other = parent->rb_left;
			if (rb_is_red(other))
			{
				rb_set_black(other);
				rb_set_red(parent);
				__rb_rotate_right(parent, root);
				other = parent->rb_left;
			}
			if ((!other->rb_left || rb_is_black(other->rb_left)) &&
			    (!other->rb_right || rb_is_black(other->rb_right)))
			{
				rb_set_red(other);
				node = parent;
				parent = rb_parent(node);
			}
			else
			{
				if (!other->rb_left || rb_is_black(other->rb_left))
				{
					register struct fio_rb_node *o_right;
					if ((o_right = other->rb_right))
						rb_set_black(o_right);
					rb_set_red(other);
					__rb_rotate_left(other, root);
					other = parent->rb_left;
				}
				rb_set_color(other, rb_color(parent));
				rb_set_black(parent);
				if (other->rb_left)
					rb_set_black(other->rb_left);
				__rb_rotate_right(parent, root);
				node = root->rb_node;
				break;
			}
		}
	}
	if (node)
		rb_set_black(node);
}

/*
 * [한국어] rb_erase - 레드-블랙 트리에서 노드를 삭제
 *
 * @node: 삭제할 노드
 * @root: 트리 루트
 *
 * 삭제할 노드의 자식 수에 따라 처리가 달라진다:
 * - 자식 0~1개: 자식으로 직접 교체
 * - 자식 2개: 중위 순회 후계자(successor)를 찾아 교체 후 후계자 위치에서 삭제
 * 삭제된 노드가 BLACK이면 __rb_erase_color()로 속성 복원.
 *
 * 호출 체인: fio 내부 코드 → [rb_erase] → __rb_erase_color
 */
void rb_erase(struct fio_rb_node *node, struct rb_root *root)
{
	struct fio_rb_node *child, *parent;
	int color;

	if (!node->rb_left)
		child = node->rb_right;
	else if (!node->rb_right)
		child = node->rb_left;
	else
	{
		struct fio_rb_node *old = node, *left;

		node = node->rb_right;
		while ((left = node->rb_left) != NULL)
			node = left;
		child = node->rb_right;
		parent = rb_parent(node);
		color = rb_color(node);

		if (child)
			rb_set_parent(child, parent);
		if (parent == old) {
			parent->rb_right = child;
			parent = node;
		} else
			parent->rb_left = child;

		node->rb_parent_color = old->rb_parent_color;
		node->rb_right = old->rb_right;
		node->rb_left = old->rb_left;

		if (rb_parent(old))
		{
			if (rb_parent(old)->rb_left == old)
				rb_parent(old)->rb_left = node;
			else
				rb_parent(old)->rb_right = node;
		} else
			root->rb_node = node;

		rb_set_parent(old->rb_left, node);
		if (old->rb_right)
			rb_set_parent(old->rb_right, node);
		goto color;
	}

	parent = rb_parent(node);
	color = rb_color(node);

	if (child)
		rb_set_parent(child, parent);
	if (parent)
	{
		if (parent->rb_left == node)
			parent->rb_left = child;
		else
			parent->rb_right = child;
	}
	else
		root->rb_node = child;

 color:
	if (color == RB_BLACK)
		__rb_erase_color(child, parent, root);
}

/*
 * This function returns the first node (in sort order) of the tree.
 */
struct fio_rb_node *rb_first(struct rb_root *root)
{
	struct fio_rb_node	*n;

	n = root->rb_node;
	if (!n)
		return NULL;
	while (n->rb_left)
		n = n->rb_left;
	return n;
}

struct fio_rb_node *rb_next(const struct fio_rb_node *node)
{
	struct fio_rb_node *parent;

	if (RB_EMPTY_NODE(node))
		return NULL;

	/*
	 * If we have a right-hand child, go down and then left as far
	 * as we can.
	 */
	if (node->rb_right) {
		node = node->rb_right; 
		while (node->rb_left)
			node=node->rb_left;
		return (struct fio_rb_node *)node;
	}

	/*
	 * No right-hand children. Everything down and left is smaller than us,
	 * so any 'next' node must be in the general direction of our parent.
	 * Go up the tree; any time the ancestor is a right-hand child of its
	 * parent, keep going up. First time it's a left-hand child of its
	 * parent, said parent is our 'next' node.
	 */
	while ((parent = rb_parent(node)) && node == parent->rb_right)
		node = parent;

	return parent;
}
