/*
 * [한국어 설명] 우선순위 검색 트리(Priority Search Tree) 공개 API 헤더 (prio_tree.h)
 *
 * === 파일의 역할 ===
 * lib/prio_tree.c 가 구현한 McCreight 1985 우선순위 검색 트리의 노드/루트/
 * 이터레이터 구조체, 초기화 매크로, 그리고 삽입/교체/제거/범위 순회 API 를
 * 노출한다. PST 는 "구간(start..last)" 을 담는 노드들의 집합에서 "주어진
 * 쿼리 구간 [r, h] 와 겹치는 모든 노드" 를 O(log N + k) 로 순회(stabbing
 * query) 하는 자료구조이다. 내부는 radix 인덱스(= start) 로 이진트리 골격
 * 을 만들고, heap 인덱스(= last) 로 힙 성질(부모 last ≥ 자식 last) 을
 * 유지하는 하이브리드 구조. fio 에서는 gfio 의 graph.c 가 그래프 위 x 좌표
 * 구간(데이터 포인트 범위) 에 대한 마우스 호버 "어느 점이 이 x 에 속하는가"
 * 쿼리에 사용한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * gfio(GTK GUI) 의 그래프 상호작용 레이어. 본 자료구조는 fio 엔진/잡
 * 경로와는 직접 연관이 없으며, 완전히 gfio 내부의 화면 렌더/툴팁 표시
 * 보조 수단이다.
 * 호출 체인(gfio/graph.c):
 *   그래프 그리기 → 각 데이터 포인트 추가 → prio_tree_insert
 *   마우스 호버 → prio_tree_iter_init(iter, &root, x, x) → prio_tree_next(iter)
 *     → 겹치는 포인트 수집 → 툴팁 표시.
 *
 * === 타 모듈과의 연결 ===
 * - prio_tree.c : 구현. McCreight 논문의 회전/리벨런스/힙 조정 로직.
 * - gfio/graph.c : 유일한 주요 소비자.
 * - <inttypes.h> : uint64_t(범위 경계).
 * 데이터 흐름: (start, last) 쌍을 갖는 사용자 객체 → prio_tree_node 필드를
 * 임베드 → insert → iter_init 으로 쿼리 범위 설정 → next 반복 → 사용자 객체
 * 복원은 prio_tree_entry 매크로(container_of 패턴) 로.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct prio_tree_node : left/right/parent 포인터 + start/last 구간.
 * - struct prio_tree_root : 루트 노드 + index_bits(트리 높이 한계).
 * - struct prio_tree_iter : 순회 상태(cur, mask, value, size_level) + 쿼리 구간.
 * - INIT_PRIO_TREE_{ROOT,NODE,ITER} : 센티널 초기화 매크로.
 * - prio_tree_empty / root / left_empty / right_empty : 상태 확인 인라인.
 * - prio_tree_entry : 노드 포인터로부터 포함 구조체 역참조 매크로.
 * - prio_tree_insert / prio_tree_replace / prio_tree_remove / prio_tree_next :
 *   핵심 조작 API.
 */

#ifndef _LINUX_PRIO_TREE_H
#define _LINUX_PRIO_TREE_H
/* [한국어] 헤더 가드 이름이 _LINUX_PRIO_TREE_H 인 것은 Linux 커널의 동명
 * 헤더에서 포팅되었음을 반영한다(과거 mm/prio_tree.c 가 원전). */

#include <inttypes.h>
/* [한국어] <inttypes.h> : uint64_t(start/last/h_index/r_index). 구간이 64비트
 * 오프셋을 다룰 수 있어야 함(대형 파일 표현). */

struct prio_tree_node {
	struct prio_tree_node	*left;
	/* [한국어] 왼쪽 자식 포인터. "자기 자신" 포인터이면 "비어 있음" 의
	 * 센티널 표기(INIT_PRIO_TREE_NODE). NULL 대신 self-ref 를 쓰는 이유는
	 * prio_tree_left_empty 같은 검사를 분기 없이 할 수 있게 하기 위함. */

	struct prio_tree_node	*right;
	/* [한국어] 오른쪽 자식. 같은 센티널 규약. */

	struct prio_tree_node	*parent;
	/* [한국어] 부모 포인터. 자기 자신이면 "루트" 의미(prio_tree_root 검사).
	 * NULL 대신 self-ref 로 루트 노드도 일반 노드 연산이 분기 없이 수행 가능. */

	uint64_t		start;
	/* [한국어] 구간의 시작 (radix_index). 트리의 이진 분기는 이 값의 비트
	 * 패턴에 따라 이뤄진다.
	 * 설정자: prio_tree_insert 호출 전 사용자가 채움.
	 * 읽는 자: insert/iter 내부 비교. */

	uint64_t		last;
	/* [한국어] 구간의 끝 (heap_index, start ≤ last). 부모 last ≥ 자식 last
	 * 힙 성질을 유지하며 삽입 시 힙 복원 연산이 일어난다.
	 * 설정자/읽는 자: 위와 동일. */
};

struct prio_tree_root {
	struct prio_tree_node	*prio_tree_node;
	/* [한국어] 트리의 루트 노드. NULL 이면 빈 트리.
	 * 설정자: INIT_PRIO_TREE_ROOT 또는 첫 insert.
	 * 읽는 자: prio_tree_empty, iter_init. */

	unsigned short 		index_bits;
	/* [한국어] 트리 높이 결정 상수. max_heap_index < 2^index_bits 를 보장.
	 * 삽입 시 heap index 가 커지면 index_bits 가 확장됨.
	 * 초기값 1(INIT_PRIO_TREE_ROOT 가 설정). 읽는 자: iter_init/next 의
	 * 마스크/value 계산. */
};

struct prio_tree_iter {
	struct prio_tree_node	*cur;
	/* [한국어] 현재 순회 위치 노드 포인터. NULL 이면 순회 시작 전 또는
	 * 종료 후. 설정자: prio_tree_iter_init(NULL 로), prio_tree_next(진행). */

	unsigned long		mask;
	/* [한국어] 현재 레벨에서의 비트 마스크. size_level 과 함께 트리의 어느
	 * 레벨에 있는지 추적. 설정자/읽는 자: prio_tree_next 내부 상태 기계. */

	unsigned long		value;
	/* [한국어] 지금까지 누적된 인덱스 값(현재 경로의 radix index 재구성).
	 * 설정자/읽는 자: prio_tree_next. */

	int			size_level;
	/* [한국어] 0 = radix 인덱스 단계(start 기준 이진 분기), > 0 = size
	 * 인덱스 단계(heap index 기반 힙 이동 중). McCreight 알고리즘의 두
	 * 단계 전이 상태 플래그. */

	struct prio_tree_root	*root;
	/* [한국어] 검색 대상 트리 루트. iter_init 에서 고정되고 next 동안 불변. */

	uint64_t		r_index;
	/* [한국어] 쿼리 구간의 시작(radix_index). prio_tree_iter_init 에서 설정.
	 * 읽는 자: prio_tree_next 가 노드 start 와 비교. */

	uint64_t		h_index;
	/* [한국어] 쿼리 구간의 끝(heap_index). 설정자: iter_init. 읽는 자:
	 * prio_tree_next 가 노드 last 와 비교. */
};

static inline void prio_tree_iter_init(struct prio_tree_iter *iter,
		struct prio_tree_root *root, uint64_t r_index, uint64_t h_index)
{
	iter->root = root;
	/* [한국어] 쿼리할 트리 루트를 바인딩. 이후 next 호출 중 불변. */
	iter->r_index = r_index;
	/* [한국어] 쿼리 시작. */
	iter->h_index = h_index;
	/* [한국어] 쿼리 끝. 보통 r_index == h_index 이면 특정 점 stabbing. */
	iter->cur = NULL;
	/* [한국어] 순회 시작 전 상태. 첫 prio_tree_next 호출이 루트로 진입. */
}

#define INIT_PRIO_TREE_ROOT(ptr)	\
do {					\
	(ptr)->prio_tree_node = NULL;	\
	(ptr)->index_bits = 1;		\
} while (0)
/* [한국어] 루트 초기화 매크로. prio_tree_node=NULL(빈 트리), index_bits=1
 * (트리 높이 최소). do-while(0) 이디엄은 매크로 내부 복수 문장을 단일 문장
 * 처럼 쓸 수 있게 하고 if/else 구문 안전성 확보. */

#define INIT_PRIO_TREE_NODE(ptr)				\
do {								\
	(ptr)->left = (ptr)->right = (ptr)->parent = (ptr);	\
} while (0)
/* [한국어] 노드 초기화 매크로. 세 포인터를 모두 self-ref 로 설정해 "연결
 * 안 된 고립 노드" 임을 표현. prio_tree_empty / prio_tree_left_empty 류
 * 검사와 호환. */

#define INIT_PRIO_TREE_ITER(ptr)	\
do {					\
	(ptr)->cur = NULL;		\
	(ptr)->mask = 0UL;		\
	(ptr)->value = 0UL;		\
	(ptr)->size_level = 0;		\
} while (0)
/* [한국어] 이터레이터 초기화. 순회 상태를 깨끗한 시작 상태로 설정.
 * prio_tree_iter_init 과 함께 root/r_index/h_index 를 별도 설정해야 실제
 * 순회 가능. */

#define prio_tree_entry(ptr, type, member) \
       ((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))
/* [한국어] container_of 패턴의 매크로 구현. prio_tree_node 포인터 ptr 에서
 * 그것을 임베드하고 있는 상위 구조체(type) 의 포인터를 복원한다.
 * 원리: type 의 member 필드가 type 시작에서 얼마나 떨어져 있는지(offsetof
 * 의 수동 계산) 를 ptr 에서 빼면 상위 구조체 시작 주소가 나옴. */

static inline int prio_tree_empty(const struct prio_tree_root *root)
{
	return root->prio_tree_node == NULL;
	/* [한국어] 루트 포인터가 NULL 이면 빈 트리. insert 가 한 번이라도 불리면
	 * 더 이상 NULL 이 아니다. */
}

static inline int prio_tree_root(const struct prio_tree_node *node)
{
	return node->parent == node;
	/* [한국어] parent 가 self-ref 이면 루트 노드. INIT_PRIO_TREE_NODE 가
	 * 고립 노드도 self-ref 로 초기화하므로 "연결된 후" 판정을 내릴 때는
	 * 이와 함께 left/right 상태도 봐야 한다(실제 로직은 prio_tree.c 내부). */
}

static inline int prio_tree_left_empty(const struct prio_tree_node *node)
{
	return node->left == node;
	/* [한국어] 왼쪽 자식이 self-ref 이면 "왼쪽 비어 있음". 순회 중 분기
	 * 결정에 사용. */
}

static inline int prio_tree_right_empty(const struct prio_tree_node *node)
{
	return node->right == node;
	/* [한국어] 오른쪽 자식이 self-ref 이면 "오른쪽 비어 있음". */
}


struct prio_tree_node *prio_tree_replace(struct prio_tree_root *root,
                struct prio_tree_node *old, struct prio_tree_node *node);
/* [한국어] prio_tree_replace - 트리 내 old 노드를 node 로 교체. 같은 구간을
 * 갖는 새 노드가 들어올 때 회전 없이 포인터만 교체하여 O(1) 에 수행.
 * 반환: 대체된 old 포인터(호출자가 해제/재사용). */

struct prio_tree_node *prio_tree_insert(struct prio_tree_root *root,
                struct prio_tree_node *node);
/* [한국어] prio_tree_insert - node(사용자가 start/last 를 채운 상태) 를
 * 트리에 삽입. 힙 성질을 유지하기 위해 경로 상 필요한 회전 수행.
 * 반환: 삽입된 노드 포인터(동일 start 가 이미 있으면 기존 노드 반환 —
 * 호출자가 중복 처리 결정). */

void prio_tree_remove(struct prio_tree_root *root, struct prio_tree_node *node);
/* [한국어] prio_tree_remove - 트리에서 node 를 제거. 이후 호출자는
 * INIT_PRIO_TREE_NODE 로 다시 초기화하거나 해제. */

struct prio_tree_node *prio_tree_next(struct prio_tree_iter *iter);
/* [한국어] prio_tree_next - iter 가 지정한 쿼리 구간 [r_index, h_index] 와
 * 겹치는 다음 노드를 반환. 순회 상태(iter->cur, mask, value, size_level) 를
 * 전진시키고, 남은 겹침 노드가 없으면 NULL 반환.
 * 호출 패턴: iter_init → while((n = prio_tree_next(&iter))) { ... }.
 * 실행 컨텍스트: gfio UI 스레드(일반적으로 단일). */

#endif /* _LINUX_PRIO_TREE_H */
/* [한국어] 가드 종료. */
