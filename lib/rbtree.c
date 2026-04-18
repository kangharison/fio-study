/*
 * [한국어 설명] 레드-블랙 트리(Red-Black Tree) 구현 (rbtree.c)
 *
 * === 파일의 역할 ===
 * Linux 커널의 `linux/lib/rbtree.c` 를 그대로 포팅한 자가 균형 이진 탐색 트리이다.
 * 5개의 레드-블랙 속성(루트 BLACK, NULL 은 BLACK, RED 는 자식 모두 BLACK,
 * 모든 루트-리프 경로의 BLACK 노드 수 동일) 을 유지하며, 삽입/삭제 시 O(log n)
 * 내에 회전과 색상 재조정으로 속성을 복구한다. 포인터 하위 비트(정렬 보장)에
 * 색상 비트를 인코딩(`rb_parent_color`)하여 메모리 오버헤드를 줄인다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 전반에서 "정렬된 키-값 매핑" 이 필요한 곳에 사용된다. 주요 사용처:
 *
 *   - iolog.c : I/O 로그 replay 모드에서 시간/오프셋 순 io_piece 를
 *               rb_tree 에 삽입하여 순차 처리 (rb_first/rb_next 로 중위 순회).
 *   - io_u.c / verify.c : 쓰기된 블록 오프셋을 정렬된 구조로 추적하여 verify 시
 *               오프셋별 메타 검색.
 *   - stat.c / client.c : 네트워크 서버 모드에서 클라이언트별 누적 통계 노드를
 *               소켓별 키로 관리.
 *   - thread_data 의 일부 보조 자료구조 (예: 트리 인덱싱된 io_piece).
 *
 * 본 파일은 "일반 rb_tree 프리미티브" 만 제공하고, 노드에 어떤 데이터를 붙이고
 * 어떤 비교 함수를 쓸지는 상위에서 결정한다(사용자 정의 struct 안에 fio_rb_node
 * 를 임베드하고 container_of 패턴으로 본체에 접근).
 *
 * 실행 컨텍스트: 각 tree 는 특정 컨텍스트(잡 스레드 / helper / main) 에서만
 * 접근되며, 공용 트리는 상위에서 락으로 보호한다. 본 파일 자체는 lock-free 가 아니다.
 *
 * === 타 모듈과의 연결 ===
 * - rbtree.h : struct fio_rb_node / rb_root 정의, rb_parent/rb_color/rb_is_red 등
 *              비트 조작 inline 헬퍼, RB_RED/RB_BLACK 매크로, RB_EMPTY_NODE/RB_CLEAR_NODE.
 * - iolog.c / rate-submit.c / stat.c : 본 파일의 rb_insert_color/rb_erase/rb_first/rb_next 소비자.
 * - 헤더는 container_of 매크로를 통해 임베드된 사용자 구조체 접근을 허용한다.
 * - 색상 인코딩 트릭: rb_parent_color 는 "부모 포인터 | color 비트(0=RED, 1=BLACK)".
 *   fio_rb_node 는 __aligned(sizeof(long)) 으로 2-바이트 정렬이 보장되므로 하위 1비트를 재활용.
 *
 * === 주요 함수/구조체 요약 ===
 * - rb_insert_color(node, root): 삽입된 RED 노드 기준 3케이스(삼촌 RED/안쪽/바깥쪽) 로 속성 복구.
 * - rb_erase(node, root): 자식 수에 따라 직접 교체 또는 후계자(successor) 교체 후 __rb_erase_color 호출.
 * - __rb_erase_color(child, parent, root): BLACK 제거로 생긴 "double black" 을 4케이스로 재배치/회전.
 * - __rb_rotate_left / __rb_rotate_right: 두 노드의 부모-자식 관계를 회전시켜 높이 균형 조정.
 * - rb_first(root): 트리의 최솟값(가장 왼쪽 노드) 반환 — 중위 순회 시작점.
 * - rb_next(node): 중위 순회의 다음 노드 반환 — 오른쪽 서브트리 최소 또는 부모 위로 올라가며 첫 왼쪽 분기.
 * - struct fio_rb_node (rbtree.h): {rb_parent_color, rb_right, rb_left} — 색상은 parent 포인터 LSB.
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

#include "rbtree.h"	/* [한국어] struct fio_rb_node, struct rb_root, rb_parent/rb_color/rb_set_* 인라인 헬퍼, RB_RED/RB_BLACK 정의 */

/*
 * [한국어] __rb_rotate_left - 노드를 기준으로 좌회전하여 높이 불균형 보정
 *
 * @node: 회전 대상 (회전 후 오른쪽 자식의 왼쪽 자식이 됨)
 * @root: 트리 루트 (회전이 루트 수준에서 일어난 경우 root->rb_node 갱신)
 *
 * 동작:
 *     node              right
 *    /    \     →      /     \
 *   T1   right       node    T3
 *        /  \        /   \
 *       T2   T3     T1    T2
 *
 * 부모 포인터를 올바르게 재연결하는 것이 핵심:
 *   1. right->rb_left 를 node->rb_right 로 옮기고 부모를 node 로 설정.
 *   2. right->rb_left = node 로 설정.
 *   3. right 의 부모를 기존 node 의 부모로 설정.
 *   4. 기존 부모의 자식 슬롯(좌/우) 을 right 로 교체. 부모가 없으면(루트) root->rb_node 갱신.
 *   5. node 의 부모를 right 로 재설정.
 *
 * 실행 컨텍스트: rb_insert_color / __rb_erase_color 내부에서만 호출.
 */
static void __rb_rotate_left(struct fio_rb_node *node, struct rb_root *root)
{
	struct fio_rb_node *right = node->rb_right;		/* [한국어] 회전 축 — 새 서브트리 루트가 될 노드 */
	struct fio_rb_node *parent = rb_parent(node);		/* [한국어] node 의 현재 부모 (회전 후 right 의 부모가 됨) */

	if ((node->rb_right = right->rb_left))			/* [한국어] right 의 왼쪽 서브트리를 node 의 오른쪽 자리로 옮김, 할당+검사 동시 수행 */
		rb_set_parent(right->rb_left, node);		/* [한국어] 옮겨진 서브트리의 부모를 node 로 재설정 (NULL 이 아닐 때만) */
	right->rb_left = node;					/* [한국어] node 를 right 의 왼쪽 자식으로 끌어내림 */

	rb_set_parent(right, parent);				/* [한국어] right 의 부모를 기존 node 의 부모로 승격 */

	if (parent)						/* [한국어] node 가 루트가 아니었으면 부모의 자식 포인터 갱신 */
	{
		if (node == parent->rb_left)			/* [한국어] node 가 왼쪽 자식이었다면 */
			parent->rb_left = right;		/* [한국어] 왼쪽 자리를 right 로 교체 */
		else
			parent->rb_right = right;		/* [한국어] 오른쪽 자리 교체 */
	}
	else
		root->rb_node = right;				/* [한국어] node 가 루트였으면 트리 루트를 right 로 교체 */
	rb_set_parent(node, right);				/* [한국어] node 의 부모를 right 로 설정 (회전 완료) */
}

/*
 * [한국어] __rb_rotate_right - 노드를 기준으로 우회전 (좌회전의 대칭 연산)
 *
 * 좌회전을 거울 반전한 것이며 각 단계의 의미가 대응된다. 주석 생략 부분은
 * __rb_rotate_left 의 대응 라인을 참조하면 된다.
 */
static void __rb_rotate_right(struct fio_rb_node *node, struct rb_root *root)
{
	struct fio_rb_node *left = node->rb_left;		/* [한국어] 회전 축 */
	struct fio_rb_node *parent = rb_parent(node);		/* [한국어] 기존 부모 */

	if ((node->rb_left = left->rb_right))			/* [한국어] left 의 오른쪽 서브트리를 node 의 왼쪽 자리로 이동 */
		rb_set_parent(left->rb_right, node);		/* [한국어] 옮긴 서브트리의 부모를 node 로 갱신 */
	left->rb_right = node;					/* [한국어] node 를 left 의 오른쪽 자식으로 끌어내림 */

	rb_set_parent(left, parent);				/* [한국어] left 를 기존 node 의 부모에 연결 */

	if (parent)						/* [한국어] 기존 node 가 루트가 아니었으면 */
	{
		if (node == parent->rb_right)			/* [한국어] node 가 오른쪽 자식이었으면 */
			parent->rb_right = left;		/* [한국어] 오른쪽 자리 교체 */
		else
			parent->rb_left = left;			/* [한국어] 왼쪽 자리 교체 */
	}
	else
		root->rb_node = left;				/* [한국어] 루트였으면 트리 루트 갱신 */
	rb_set_parent(node, left);				/* [한국어] node 의 부모를 left 로 설정 */
}

/*
 * [한국어] rb_insert_color - 삽입 후 레드-블랙 속성을 복원
 *
 * @node: 새로 삽입된 노드 (호출자는 rb_link_node 로 연결 + RED 로 초기화한 상태)
 * @root: 트리 루트
 *
 * 배경: 신규 노드를 RED 로 삽입하면 "모든 루트-리프 경로의 BLACK 수 동일" 은
 * 유지되지만, 부모도 RED 인 경우 "RED 의 자식은 BLACK" 속성을 위반한다.
 * 삼촌(uncle = 부모의 형제) 색깔에 따라 3케이스로 복구한다:
 *
 *   Case 1: 삼촌이 RED
 *     → 부모와 삼촌을 BLACK 으로, 조부모를 RED 로. 조부모가 루트에서 또 위반일 수
 *       있으므로 node=조부모 로 바꾸고 루프 위로.
 *   Case 2: 삼촌이 BLACK + node 가 부모의 "안쪽" 자식 (지그재그)
 *     → 부모를 기준으로 회전하여 Case 3 로 변환.
 *   Case 3: 삼촌이 BLACK + node 가 부모의 "바깥쪽" 자식 (직선)
 *     → 부모를 BLACK, 조부모를 RED 로 하고 조부모를 반대 방향 회전.
 *
 * 왼쪽(부모가 조부모의 왼쪽 자식)/오른쪽 대칭 케이스가 거울 반전으로 이어진다.
 * 마지막에 루트를 BLACK 으로 강제(속성 2).
 *
 * 실행 컨텍스트: 삽입 경로(사용자 쓰레드 또는 main). 트리 접근 락은 상위 책임.
 */
void rb_insert_color(struct fio_rb_node *node, struct rb_root *root)
{
	struct fio_rb_node *parent, *gparent;	/* [한국어] 각각 부모/조부모 임시 포인터 */

	/* [한국어] 부모가 RED 인 동안 반복하여 속성 위반을 수정 (node 자체는 RED 상태) */
	while ((parent = rb_parent(node)) && rb_is_red(parent))
	{
		gparent = rb_parent(parent);	/* [한국어] 조부모는 parent->parent. 여기서 parent 가 RED 이므로 루트 아님 → gparent 존재 보장 */

		if (parent == gparent->rb_left)	/* [한국어] 왼쪽 대칭 케이스 */
		{
			{
				/* [한국어] 케이스 1: 삼촌이 RED → 색상만 변경하고 조부모로 상향 */
				register struct fio_rb_node *uncle = gparent->rb_right;	/* [한국어] register 힌트: 자주 쓰이는 로컬 포인터, 컴파일러에 레지스터 할당 제안 */
				if (uncle && rb_is_red(uncle))
				{
					rb_set_black(uncle);		/* [한국어] 삼촌 BLACK */
					rb_set_black(parent);		/* [한국어] 부모 BLACK */
					rb_set_red(gparent);		/* [한국어] 조부모 RED (위반이 위로 전파될 수 있음) */
					node = gparent;			/* [한국어] 상향: 다음 루프에서 조부모를 기준으로 재검사 */
					continue;
				}
			}

			/* [한국어] 케이스 2: 안쪽 자식 (지그재그) → 좌회전으로 케이스 3 로 변환 */
			if (parent->rb_right == node)
			{
				register struct fio_rb_node *tmp;
				__rb_rotate_left(parent, root);		/* [한국어] parent 기준 좌회전: node 와 parent 의 역할이 교환됨 */
				tmp = parent;				/* [한국어] 이후 처리를 위해 node/parent 스왑 */
				parent = node;
				node = tmp;
			}

			/* [한국어] 케이스 3: 바깥 자식 (직선) → 부모 BLACK, 조부모 RED 로 변경 후 우회전 */
			rb_set_black(parent);
			rb_set_red(gparent);
			__rb_rotate_right(gparent, root);	/* [한국어] 조부모 기준 우회전으로 균형 회복 */
		} else {
			{
				/* [한국어] 대칭 케이스: 부모가 조부모의 오른쪽 자식인 경우 */
				register struct fio_rb_node *uncle = gparent->rb_left;	/* [한국어] 삼촌 = 조부모의 왼쪽 자식 */
				if (uncle && rb_is_red(uncle))
				{
					rb_set_black(uncle);		/* [한국어] 케이스 1 대칭 */
					rb_set_black(parent);
					rb_set_red(gparent);
					node = gparent;
					continue;
				}
			}

			if (parent->rb_left == node)		/* [한국어] 케이스 2 대칭: 안쪽 자식 → 우회전 */
			{
				register struct fio_rb_node *tmp;
				__rb_rotate_right(parent, root);
				tmp = parent;
				parent = node;
				node = tmp;
			}

			/* [한국어] 케이스 3 대칭 */
			rb_set_black(parent);
			rb_set_red(gparent);
			__rb_rotate_left(gparent, root);	/* [한국어] 조부모 기준 좌회전 */
		}
	}

	/* [한국어] 루트는 항상 BLACK 이어야 함 (레드-블랙 속성 2). 케이스 1 반복으로
	 * 루트가 RED 가 되었을 수 있으므로 명시적으로 BLACK 강제. */
	rb_set_black(root->rb_node);
}

/*
 * [한국어] __rb_erase_color - 삭제 후 레드-블랙 트리 속성 복원 (BLACK 제거가 유발한 "double black")
 *
 * @node:   삭제된 노드를 대체한 자식 (NULL 가능 — "nil BLACK 리프" 로 간주)
 * @parent: 해당 자리(node 의 상위). NULL 이 아님 — 루트 자체가 사라진 경우 호출되지 않음.
 * @root:   트리 루트
 *
 * 처리: 형제(sibling = 반대쪽 자식) 의 색과 형제의 자식들 색에 따라 4 케이스:
 *   Case 1: 형제가 RED → 회전으로 형제를 BLACK 으로 만드는 표준 변형.
 *   Case 2: 형제 BLACK + 형제의 두 자식 모두 BLACK → 형제를 RED 로 하고 parent 로 올라가 재시도.
 *   Case 3: 형제 BLACK + 바깥 조카 BLACK + 안쪽 조카 RED → 형제 기준 회전으로 Case 4 변환.
 *   Case 4: 형제 BLACK + 바깥 조카 RED → 부모 기준 회전 + 색상 재배치로 종료.
 *
 * 마지막에 node != NULL 이면 BLACK 으로 강제(루트 승격 시 속성 2 유지).
 */
static void __rb_erase_color(struct fio_rb_node *node,
			     struct fio_rb_node *parent,
			     struct rb_root *root)
{
	struct fio_rb_node *other;	/* [한국어] sibling 임시 포인터 */

	while ((!node || rb_is_black(node)) && node != root->rb_node)	/* [한국어] node 가 NULL 또는 BLACK 인 동안 루프; 루트 도달 시 종료 */
	{
		if (parent->rb_left == node)		/* [한국어] node 가 부모의 왼쪽 자식 → sibling = 오른쪽 */
		{
			other = parent->rb_right;	/* [한국어] 형제 */
			if (rb_is_red(other))		/* [한국어] 케이스 1: 형제 RED → 회전으로 BLACK 형제 만들기 */
			{
				rb_set_black(other);		/* [한국어] 형제 BLACK */
				rb_set_red(parent);		/* [한국어] 부모 RED (교체) */
				__rb_rotate_left(parent, root);	/* [한국어] 부모 기준 좌회전 */
				other = parent->rb_right;	/* [한국어] 회전 후 새 형제 갱신 */
			}
			if ((!other->rb_left || rb_is_black(other->rb_left)) &&
			    (!other->rb_right || rb_is_black(other->rb_right)))	/* [한국어] 케이스 2: 두 조카 모두 BLACK */
			{
				rb_set_red(other);		/* [한국어] 형제를 RED 로 → "BLACK 짐" 을 부모로 올림 */
				node = parent;			/* [한국어] 상향 */
				parent = rb_parent(node);	/* [한국어] 새 부모 */
			}
			else
			{
				if (!other->rb_right || rb_is_black(other->rb_right))	/* [한국어] 케이스 3: 바깥 조카 BLACK + 안쪽 조카 RED */
				{
					struct fio_rb_node *o_left;
					if ((o_left = other->rb_left))
						rb_set_black(o_left);		/* [한국어] 안쪽 조카를 BLACK */
					rb_set_red(other);			/* [한국어] 형제 RED */
					__rb_rotate_right(other, root);	/* [한국어] 형제 기준 우회전 → 케이스 4 변환 */
					other = parent->rb_right;	/* [한국어] 새 형제 */
				}
				/* [한국어] 케이스 4: 부모 기준 회전 + 색상 재배치로 종료 */
				rb_set_color(other, rb_color(parent));	/* [한국어] 형제는 부모의 색 상속 */
				rb_set_black(parent);			/* [한국어] 부모 BLACK */
				if (other->rb_right)
					rb_set_black(other->rb_right);	/* [한국어] 바깥 조카 BLACK */
				__rb_rotate_left(parent, root);	/* [한국어] 부모 기준 좌회전 */
				node = root->rb_node;		/* [한국어] 종료 조건 강제 (루트 지정) */
				break;
			}
		}
		else					/* [한국어] 대칭 케이스: node 가 오른쪽 자식 → sibling = 왼쪽 */
		{
			other = parent->rb_left;
			if (rb_is_red(other))		/* [한국어] 케이스 1 대칭 */
			{
				rb_set_black(other);
				rb_set_red(parent);
				__rb_rotate_right(parent, root);
				other = parent->rb_left;
			}
			if ((!other->rb_left || rb_is_black(other->rb_left)) &&
			    (!other->rb_right || rb_is_black(other->rb_right)))	/* [한국어] 케이스 2 대칭 */
			{
				rb_set_red(other);
				node = parent;
				parent = rb_parent(node);
			}
			else
			{
				if (!other->rb_left || rb_is_black(other->rb_left))	/* [한국어] 케이스 3 대칭 */
				{
					register struct fio_rb_node *o_right;
					if ((o_right = other->rb_right))
						rb_set_black(o_right);
					rb_set_red(other);
					__rb_rotate_left(other, root);
					other = parent->rb_left;
				}
				/* [한국어] 케이스 4 대칭 */
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
		rb_set_black(node);		/* [한국어] 최종적으로 node 가 존재하면 BLACK 으로 강제 (루트 BLACK 속성/더블블랙 해소) */
}

/*
 * [한국어] rb_erase - 레드-블랙 트리에서 노드를 삭제
 *
 * @node: 삭제할 노드 (호출자가 트리에 실제로 속해있다고 확인한 포인터)
 * @root: 트리 루트
 *
 * 케이스:
 *   (a) 왼쪽 자식 없음  → node 를 오른쪽 자식으로 대체.
 *   (b) 오른쪽 자식 없음 → node 를 왼쪽 자식으로 대체.
 *   (c) 양쪽 자식 존재  → 중위 순회 후계자(오른쪽 서브트리의 가장 왼쪽)를 찾아
 *                         node 자리로 복사해 넣고, 후계자 위치에서 다시 (a)/(b) 처리.
 *
 * 삭제된 "실질 노드" 가 BLACK 이면 __rb_erase_color 로 속성 복구.
 *
 * 실행 컨텍스트: 잡/메인 스레드. 트리 락은 상위 책임.
 */
void rb_erase(struct fio_rb_node *node, struct rb_root *root)
{
	struct fio_rb_node *child, *parent;	/* [한국어] child=실제 자리로 승격되는 노드, parent=그 자리의 부모 */
	int color;				/* [한국어] 실질 삭제된 노드의 색 (복구 필요 여부 판정) */

	if (!node->rb_left)			/* [한국어] (a) 왼쪽 없음 */
		child = node->rb_right;
	else if (!node->rb_right)		/* [한국어] (b) 오른쪽 없음 */
		child = node->rb_left;
	else					/* [한국어] (c) 양쪽 자식 존재 */
	{
		struct fio_rb_node *old = node, *left;

		node = node->rb_right;		/* [한국어] 오른쪽 서브트리로 진입 */
		while ((left = node->rb_left) != NULL)	/* [한국어] 가장 왼쪽(최솟값) = 중위 후계자 탐색 */
			node = left;
		child = node->rb_right;		/* [한국어] 후계자의 오른쪽 서브트리(왼쪽은 없음) */
		parent = rb_parent(node);	/* [한국어] 후계자의 부모 */
		color = rb_color(node);		/* [한국어] 후계자의 색 (실질 삭제 대상) */

		if (child)
			rb_set_parent(child, parent);	/* [한국어] child 의 부모 재연결 */
		if (parent == old) {			/* [한국어] 후계자가 old 의 직계 오른쪽 자식인 특수 케이스 */
			parent->rb_right = child;	/* [한국어] old 의 오른쪽 자리에 child 연결 */
			parent = node;			/* [한국어] 색 보정 시 parent 기준점을 후계자 자신으로 */
		} else
			parent->rb_left = child;	/* [한국어] 일반: 후계자는 부모의 왼쪽 자식이었음 */

		node->rb_parent_color = old->rb_parent_color;	/* [한국어] 후계자가 old 의 자리에 들어가며 색도 old 의 색 상속 */
		node->rb_right = old->rb_right;
		node->rb_left = old->rb_left;

		if (rb_parent(old))			/* [한국어] old 의 부모 재연결 */
		{
			if (rb_parent(old)->rb_left == old)
				rb_parent(old)->rb_left = node;
			else
				rb_parent(old)->rb_right = node;
		} else
			root->rb_node = node;		/* [한국어] old 가 루트였으면 새 루트 */

		rb_set_parent(old->rb_left, node);	/* [한국어] old 의 자식들의 부모를 node 로 갱신 */
		if (old->rb_right)
			rb_set_parent(old->rb_right, node);
		goto color;			/* [한국어] 색 복구 경로로 점프 */
	}

	parent = rb_parent(node);		/* [한국어] (a)/(b) 경로: 실질 삭제 대상은 node 자체 */
	color = rb_color(node);

	if (child)
		rb_set_parent(child, parent);	/* [한국어] child 의 부모 재연결 */
	if (parent)
	{
		if (parent->rb_left == node)
			parent->rb_left = child;
		else
			parent->rb_right = child;
	}
	else
		root->rb_node = child;		/* [한국어] node 가 루트였으면 child 가 새 루트 */

 color:
	if (color == RB_BLACK)			/* [한국어] 실질 삭제가 BLACK 이면 속성 복구 호출 */
		__rb_erase_color(child, parent, root);
}

/*
 * This function returns the first node (in sort order) of the tree.
 */
/*
 * [한국어] rb_first - 정렬 순서상 가장 작은(최좌측) 노드 반환
 *
 * @root: 트리 루트
 * @return: 최좌측 노드 또는 빈 트리면 NULL
 *
 * 중위 순회의 시작점. for (n=rb_first(root); n; n=rb_next(n)) { ... } 패턴으로 사용.
 * 실행 컨텍스트: 잡/메인 스레드 순회.
 */
struct fio_rb_node *rb_first(struct rb_root *root)
{
	struct fio_rb_node	*n;

	n = root->rb_node;		/* [한국어] 루트에서 시작 */
	if (!n)
		return NULL;		/* [한국어] 빈 트리 */
	while (n->rb_left)
		n = n->rb_left;		/* [한국어] 가장 왼쪽까지 내려감 (= 최솟값) */
	return n;
}

/*
 * [한국어] rb_next - 중위 순회(in-order) 상 다음 노드 반환
 *
 * @node: 현재 노드 (rb_first 등으로 얻은 유효 노드)
 * @return: 다음 노드 또는 끝이면 NULL
 *
 * 규칙:
 *   1. 오른쪽 자식이 있으면 오른쪽 → 왼쪽으로 내려가는 경로의 가장 왼쪽이 다음.
 *   2. 없으면 부모로 올라가되, 현재 노드가 부모의 "오른쪽" 자식인 동안 계속 상향.
 *      처음으로 "왼쪽" 자식으로 올라가는 순간 그 부모가 다음.
 *
 * 이 로직은 모든 중위 후계자를 O(평균 1 홉, 최악 O(log n)) 으로 반환한다.
 */
struct fio_rb_node *rb_next(const struct fio_rb_node *node)
{
	struct fio_rb_node *parent;

	if (RB_EMPTY_NODE(node))	/* [한국어] 분리된(트리에 없는) 노드는 next 없음 */
		return NULL;

	/*
	 * If we have a right-hand child, go down and then left as far
	 * as we can.
	 */
	if (node->rb_right) {		/* [한국어] 규칙 1: 오른쪽 서브트리의 최좌측으로 */
		node = node->rb_right;
		while (node->rb_left)
			node=node->rb_left;
		return (struct fio_rb_node *)node;	/* [한국어] const 캐스트 제거 (API 규약상 비 const 반환) */
	}

	/*
	 * No right-hand children. Everything down and left is smaller than us,
	 * so any 'next' node must be in the general direction of our parent.
	 * Go up the tree; any time the ancestor is a right-hand child of its
	 * parent, keep going up. First time it's a left-hand child of its
	 * parent, said parent is our 'next' node.
	 */
	while ((parent = rb_parent(node)) && node == parent->rb_right)	/* [한국어] 규칙 2: 오른쪽 자식 체인을 끝까지 상향 */
		node = parent;

	return parent;			/* [한국어] 처음으로 "왼쪽 자식" 이었을 때의 부모 — 루트 넘어섰으면 NULL */
}
