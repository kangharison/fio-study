/*
 * [한국어 설명] Linux 커널 유래 레드-블랙 트리 공개 API 헤더 (rbtree.h)
 *
 * === 파일의 역할 ===
 * lib/rbtree.c 가 구현한 Andrea Arcangeli 의 Linux 커널 레드-블랙 트리를
 * fio 컨텍스트에 포팅한 공개 API 를 제공한다. 노드 구조(struct fio_rb_node),
 * 루트(struct rb_root), 초기화 매크로(RB_ROOT, RB_EMPTY_ROOT/NODE, RB_CLEAR_NODE),
 * 색상/부모 인코딩 접근자 매크로(rb_parent, rb_color, rb_is_red/black,
 * rb_set_red/black, rb_set_parent, rb_set_color), 항목 복원 매크로(rb_entry =
 * container_of), 그리고 핵심 조작 함수(rb_link_node, rb_insert_color, rb_erase,
 * rb_first, rb_next) 를 노출한다. 삽입/검색의 비교 로직은 호출자가 구현
 * 하고, 본 라이브러리는 O(log N) 균형 유지만 담당하는 콜백-프리(perf 중시) 설계.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 전반의 "정렬된 컬렉션" 자료구조 기반. iolog.c 의 I/O 조각(io_piece) 을
 * 오프셋 키로 정렬, smalloc 메모리 풀의 빈 블록 관리, verify.c 의 io_hist_tree,
 * cgroup.c 의 제어 트리 등 다수 모듈이 rb_root 를 선언하고 이 API 로 노드를
 * 삽입/탐색/삭제한다. container_of 패턴으로 사용자 구조체를 임베드.
 * 호출 체인:
 *   사용자 코드: struct my_entry { ... struct fio_rb_node rb; ... };
 *     → 검색: while (n) { ... n = n->rb_left or n->rb_right; }
 *     → 삽입: rb_link_node(&e->rb, parent, link_addr) → rb_insert_color
 *     → 삭제: rb_erase(&e->rb, &root)
 *     → 순회: for (n = rb_first(&root); n; n = rb_next(n))
 *
 * === 타 모듈과의 연결 ===
 * - rbtree.c : 구현. 삽입 후 색상 복원(LL/LR/RL/RR 4 케이스 회전), 삭제
 *   후 double-black 해결.
 * - fio.h : container_of 매크로 정의(rb_entry 가 의존).
 * - iolog.c / smalloc.c / verify.c / cgroup.c 등 : 사용자 측.
 * 데이터 흐름: 사용자 구조체에 fio_rb_node 임베드 → 해당 노드 포인터로 트리
 * 조작 → rb_entry 매크로로 상위 구조체 복원.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct fio_rb_node : rb_parent_color(부모+색상 비트 팩), rb_left, rb_right.
 * - struct rb_root : rb_node 포인터만 담는 컨테이너.
 * - rb_parent_color : 부모 포인터 하위 2 비트를 색상 저장에 재사용하는 비트
 *   팩 트릭(메모리 절약). __attribute__((aligned(sizeof(long)))) 로 정렬 확보.
 * - rb_link_node : 새 노드를 rb 구조에 연결(색상 조정 전).
 * - rb_insert_color : 삽입 후 RB 불변성 복원(균형 잡힌 회전/색상 교환).
 * - rb_erase : 노드 삭제 + 불변성 복원.
 * - rb_first / rb_next : 논리적 in-order 첫 노드/다음 노드.
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
/* [한국어] 헤더 가드. Linux 커널 원본 이름을 유지. */

#include <stdlib.h>
/* [한국어] <stdlib.h> : size_t 간접 제공 + 일부 환경에서 NULL 정의. 직접
 * 쓰이는 심볼은 없지만 rbtree.c 가 malloc/free 에 의존하는 관습적 포함. */

#include <inttypes.h>
/* [한국어] <inttypes.h> : intptr_t/uintptr_t. rb_parent_color 의 비트 팩에
 * 포인터-정수 변환이 필요하며 이식성을 위해 고정폭 정수 타입 사용. */

struct fio_rb_node
{
	intptr_t rb_parent_color;
	/* [한국어] 부모 포인터와 색상 비트를 하나의 필드에 인코딩.
	 * 비트 레이아웃: 하위 2 비트 = [unused, color], 상위 비트 = 부모 포인터.
	 * 정렬 강제(aligned(sizeof(long))) 로 인해 부모 주소의 하위 2비트가
	 * 항상 0 이 되므로 이 영역을 색상 저장에 안전하게 재사용 가능 —
	 * Linux 커널의 메모리 절약 기법을 그대로 채택.
	 * 값 범위: (부모 주소 & ~3) | color(0 또는 1).
	 * 설정자: rb_link_node, rb_set_parent, rb_set_red/black.
	 * 읽는 자: rb_parent, rb_color, rb_is_red/black 매크로.
	 * 동기화: 트리 조작은 호출자가 외부 락으로 보호해야 함(본 라이브러리는
	 *   락을 제공하지 않음). iolog 는 td->io_hist_lock 사용. */
#define	RB_RED		0
	/* [한국어] 색상 상수 RED = 0. rb_parent_color 의 비트 0 이 0 이면 RED. */
#define	RB_BLACK	1
	/* [한국어] BLACK = 1. 비트 0 이 1 이면 BLACK. */

	struct fio_rb_node *rb_right;
	/* [한국어] 오른쪽 자식(키가 큰 쪽 관례). NULL = 없음.
	 * 설정자: rb_link_node 가 NULL 초기화. 이후 호출자 검색 루프가 실제 포인터로 설정.
	 * 읽는 자: 호출자 검색 + 삽입 후 rb_insert_color 내부 회전. */

	struct fio_rb_node *rb_left;
	/* [한국어] 왼쪽 자식(키가 작은 쪽 관례). NULL = 없음. */
} __attribute__((aligned(sizeof(long))));
    /* The alignment might seem pointless, but allegedly CRIS needs it */
    /* [한국어] aligned(sizeof(long)) : 노드를 word 경계로 정렬. 위 rb_parent_color
     * 의 비트 팩 트릭이 의존하는 하위 비트 0 보장. 대부분 아키텍처는 기본
     * 정렬이 이미 word 이지만 구 CRIS 포트 같은 예외를 위해 명시. */

struct rb_root
{
	struct fio_rb_node *rb_node;
	/* [한국어] 트리의 루트 노드 포인터. NULL 이면 빈 트리.
	 * 설정자: RB_ROOT 이니셜라이저 또는 첫 삽입. 읽는 자: rb_first, 호출자 검색. */
};


/* [한국어] rb_parent_color 필드에서 부모 포인터와 색상을 추출/설정하는 매크로 군.
 * 전부 비트 마스킹/OR 로 구현되어 브랜치 없음. */
#define rb_parent(r)   ((struct fio_rb_node *)((r)->rb_parent_color & ~3))
/* [한국어] 하위 2 비트를 마스크 아웃하여 부모 포인터 복원. ~3 = ...11111100. */
#define rb_color(r)   ((r)->rb_parent_color & 1)
/* [한국어] 비트 0 만 추출 → 0(RED) / 1(BLACK). */
#define rb_is_red(r)   (!rb_color(r))
/* [한국어] RED 판정. 호출자의 색상 기반 분기에 사용. */
#define rb_is_black(r) rb_color(r)
/* [한국어] BLACK 판정. */
#define rb_set_red(r)  do { (r)->rb_parent_color &= ~1; } while (0)
/* [한국어] 비트 0 을 0 으로 클리어 → RED 로 설정. 매크로 안전 이디엄(do-while). */
#define rb_set_black(r)  do { (r)->rb_parent_color |= 1; } while (0)
/* [한국어] 비트 0 을 1 로 설정 → BLACK. */

static inline void rb_set_parent(struct fio_rb_node *rb, struct fio_rb_node *p)
{
	rb->rb_parent_color = (rb->rb_parent_color & 3) | (uintptr_t)p;
	/* [한국어] 하위 2 비트(색상 등) 는 보존하고 상위에 새 부모 주소 합성.
	 * p 의 하위 2 비트는 aligned(sizeof(long)) 덕분에 0 이므로 OR 로 무손실 조합. */
}
static inline void rb_set_color(struct fio_rb_node *rb, int color)
{
	rb->rb_parent_color = (rb->rb_parent_color & ~1) | color;
	/* [한국어] 비트 0 만 교체. color 는 0 또는 1. 부모 주소/unused 비트는 보존. */
}

#define RB_ROOT	(struct rb_root) { NULL, }
/* [한국어] rb_root 인스턴스 초기화용 compound literal. struct rb_root r = RB_ROOT;
 * 로 빈 트리 초기 상태 선언. */
#define	rb_entry(ptr, type, member) container_of(ptr, type, member)
/* [한국어] container_of 의 별칭. fio_rb_node 포인터에서 그것을 임베드한 사용자
 * 구조체 복원. 예: struct io_piece *p = rb_entry(n, struct io_piece, rb_node). */

#define RB_EMPTY_ROOT(root)	((root)->rb_node == NULL)
/* [한국어] 트리 비어 있음 판정(루트 포인터가 NULL). */
#define RB_EMPTY_NODE(node)	(rb_parent(node) == node)
/* [한국어] 노드가 트리에 연결되지 않음(부모가 self-ref) 판정. 해제 전
 * 안전성 체크 용도. */
#define RB_CLEAR_NODE(node)	(rb_set_parent(node, node))
/* [한국어] 노드를 "고립" 상태로 만듦(부모 = 자기 자신). 트리에서 erase 후
 * 재사용 전에 이 상태로 두면 RB_EMPTY_NODE 로 안전 판정 가능. */

extern void rb_insert_color(struct fio_rb_node *, struct rb_root *);
/* [한국어] rb_insert_color - rb_link_node 로 삽입된 새 노드에 대해 RB 불변성
 * (1) 루트가 BLACK (2) RED 노드의 자식은 BLACK (3) 모든 경로의 BLACK 높이
 * 동일 을 복원한다. LL/LR/RL/RR 4 케이스 회전 + 색상 교환으로 O(log N).
 * 호출 순서: rb_link_node → rb_insert_color. 두 단계 분리는 "삽입 위치 검색"
 * 을 호출자 비교 로직에 맡기기 위함(콜백-프리 설계). */

extern void rb_erase(struct fio_rb_node *, struct rb_root *);
/* [한국어] rb_erase - 노드를 제거하고 RB 불변성을 복원. 삭제는 double-black
 * 해결(형제 색/조카 색에 따른 4 케이스) 알고리즘. 호출 후 노드의 메모리
 * 해제/재사용은 호출자 책임. */

/* Find logical next and previous nodes in a tree */
extern struct fio_rb_node *rb_first(struct rb_root *);
/* [한국어] rb_first - 트리에서 in-order 로 가장 작은(= 왼쪽 최하단) 노드 반환.
 * 빈 트리이면 NULL. 순회 시작점. */
extern struct fio_rb_node *rb_next(const struct fio_rb_node *);
/* [한국어] rb_next - 주어진 노드의 in-order 다음 노드. 없으면 NULL.
 * 패턴: for (n = rb_first(&root); n; n = rb_next(n)). */

/*
 * [한국어] rb_link_node - 새 노드를 트리에 연결 (색상 조정 전).
 *
 * @node: 삽입할 새 노드(사용자 구조체 내 임베드된 fio_rb_node).
 * @parent: 새 노드의 부모가 될 노드(호출자 검색 루프가 결정).
 * @rb_link: 부모의 left 또는 right 포인터의 주소(호출자 검색 루프가 결정).
 *
 * 동작: node->rb_parent_color = parent (색상 0=RED 로 세팅 효과, 정렬로 인한
 *   비트 0=0), node->rb_{left,right} = NULL, *rb_link = node.
 * 이 함수 호출 후 반드시 rb_insert_color() 를 호출해야 트리 균형이 유지된다.
 *
 * 실행 컨텍스트: 호출자 스레드. 호출자가 외부 동기화 책임. */
static inline void rb_link_node(struct fio_rb_node * node,
				struct fio_rb_node * parent,
				struct fio_rb_node ** rb_link)
{
	node->rb_parent_color = (uintptr_t)parent;
	/* [한국어] 부모 주소 세팅 + 색상 비트 0 = RED 설정(새 노드는 항상 RED
	 * 로 삽입되어야 BLACK 높이 불변 중 "모든 경로 BLACK 수" 를 즉각 위반
	 * 하지 않음). */
	node->rb_left = node->rb_right = NULL;
	/* [한국어] 새 노드는 리프로 추가되므로 두 자식 포인터 NULL. */

	*rb_link = node;
	/* [한국어] 부모의 left/right 포인터를 새 노드로 갱신. 이제 새 노드가
	 * 트리의 구조적 일부가 됨. 다음 rb_insert_color 가 색상 균형을 맞춤. */
}

#endif	/* _LINUX_RBTREE_H */
/* [한국어] 가드 종료. */
