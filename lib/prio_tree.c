/*
 * [한국어 설명] 우선순위 검색 트리(Radix Priority Search Tree) 구현 (prio_tree.c)
 *
 * === 파일의 역할 ===
 * Linux 커널 `lib/prio_tree.c` 를 포팅하여 "구간(interval) overlap query" 를
 * 효율적으로 처리하는 Radix PST 를 구현한다. 저장 항목은 닫힌 구간 [start, last]
 * (= [radix_index, heap_index])이며, 주어진 쿼리 구간과 겹치는 모든 저장 구간을
 * O(log n + m) 시간에 열거할 수 있다 (n = 총 노드 수, m = 겹치는 결과 수).
 *
 * 핵심 아이디어 (McCreight 1985, SIAM J. Computing 14(2)):
 *   - Heap ordering: 부모의 heap_index >= 자식의 heap_index (최대 힙).
 *   - Radix ordering: 비트 단위로 radix_index 의 비트 패턴에 따라 좌/우로 분기.
 *   - 두 순서를 결합하여, "쿼리 구간 [q_start, q_last]" 과 겹치는 노드들이
 *     항상 트리 상단부에 모이도록 함으로써 조기 가지치기(early termination) 가능.
 *
 * 실제 구현에서는 radix_index 중복을 허용하기 위해 `(radix_index, size=heap-radix)`
 * 를 복합 키로 사용한다. 최대 높이는 (32비트) 64, (64비트) 128 수준.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 에서는 주로 `gfio` (GTK 기반 GUI 프론트엔드) 의 그래프 조회 기능에서
 * 사용된다 (graph.c). 그래프의 x 축(시간 또는 오프셋) 상 마우스 위치에 해당하는
 * 데이터 포인트(들)를 빠르게 찾기 위해, 구간 [start, end] 를 PST 에 저장하고
 * stabbing query (쿼리 구간 [x, x] 과 겹치는 저장 구간들 열거) 를 수행한다.
 *
 * fio 의 핵심 I/O 경로(backend.c / io_u.c / ioengines.c) 에서는 직접 사용되지 않지만,
 * verify 검사에서 "파일 범위별 기록된 영역" 을 추적해야 하는 일부 모드의 후보
 * 자료구조이기도 하다. (현재 verify.c 자체는 rb_tree 기반 io_piece 를 주로 사용.)
 *
 * === 타 모듈과의 연결 ===
 * - prio_tree.h : struct prio_tree_root/node/iter 정의, BITS_PER_LONG,
 *                 INIT_PRIO_TREE_NODE/ROOT/ITER, prio_tree_empty/left_empty/right_empty/root 헬퍼.
 * - compiler/compiler.h : fio_init 매크로 — __attribute__((constructor)) 에 대응,
 *                          라이브러리 로드 시점에 prio_tree_init() 호출을 등록.
 * - assert.h, stdlib.h, limits.h : 어설션, 표준 라이브러리 (이 파일은 malloc/free 를 직접 하지 않으며
 *                                  노드 할당은 사용자가 수행; 내부 assertion 만 assert 사용).
 *
 * === 주요 함수/구조체 요약 ===
 * - prio_tree_init (constructor): index_bits_to_maxindex[] 테이블 사전 계산.
 * - prio_tree_maxindex(bits): bits 비트로 표현 가능한 최대 heap_index 반환.
 * - prio_tree_expand(root, node, max_heap): heap_index 가 현재 index_bits 로 표현 불가할 때 트리 확장.
 * - prio_tree_replace(root, old, node): old 노드 위치에 node 를 교체하고 old 반환.
 * - prio_tree_insert(root, node): 삽입. 중복(동일 radix+heap) 발견 시 기존 노드 반환.
 * - prio_tree_remove(root, node): node 를 트리에서 분리 (하위 노드 중 heap_index 큰 것으로 대체).
 * - prio_tree_first/next (pre-order 반복자): 쿼리 구간과 겹치는 노드를 O(log n + m) 으로 열거.
 * - prio_tree_left/right/parent (반복자 이동 헬퍼): iter->mask/value/size_level 을 경로 정보와 함께 갱신.
 * - overlap(iter, r, h): 쿼리 구간과 노드 [r, h] 의 겹침 여부 판정.
 * - get_index(node, *radix, *heap): 노드의 start/last 를 읽어 radix/heap 인덱스로 정규화.
 */

/*
 * lib/prio_tree.c - priority search tree
 *
 * Copyright (C) 2004, Rajesh Venkatasubramanian <vrajesh@umich.edu>
 *
 * This file is released under the GPL v2.
 *
 * Based on the radix priority search tree proposed by Edward M. McCreight
 * SIAM Journal of Computing, vol. 14, no.2, pages 257-276, May 1985
 *
 * 02Feb2004	Initial version
 */

#include <assert.h>	/* [한국어] assert() 매크로 — 트리 내부 불변식 확인 (릴리스 빌드는 NDEBUG 로 제거 가능) */
#include <stdlib.h>	/* [한국어] size_t 등 표준 타입 노출 (이 파일 내에서는 malloc 직접 사용 안 함) */
#include <limits.h>	/* [한국어] ULONG_MAX — 반복자 mask 의 "끝" 표현 상수로 사용 */

#include "../compiler/compiler.h"	/* [한국어] fio_init 매크로 (constructor 속성) — prio_tree_init 을 라이브러리 로드 시점에 호출 등록 */
#include "prio_tree.h"			/* [한국어] struct prio_tree_root / prio_tree_node / prio_tree_iter 정의 및 helper 매크로들 */

/*
 * A clever mix of heap and radix trees forms a radix priority search tree (PST)
 * which is useful for storing intervals, e.g, we can consider a vma as a closed
 * interval of file pages [offset_begin, offset_end], and store all vmas that
 * map a file in a PST. Then, using the PST, we can answer a stabbing query,
 * i.e., selecting a set of stored intervals (vmas) that overlap with (map) a
 * given input interval X (a set of consecutive file pages), in "O(log n + m)"
 * time where 'log n' is the height of the PST, and 'm' is the number of stored
 * intervals (vmas) that overlap (map) with the input interval X (the set of
 * consecutive file pages).
 *
 * In our implementation, we store closed intervals of the form [radix_index,
 * heap_index]. We assume that always radix_index <= heap_index. McCreight's PST
 * is designed for storing intervals with unique radix indices, i.e., each
 * interval have different radix_index. However, this limitation can be easily
 * overcome by using the size, i.e., heap_index - radix_index, as part of the
 * index, so we index the tree using [(radix_index,size), heap_index].
 *
 * When the above-mentioned indexing scheme is used, theoretically, in a 32 bit
 * machine, the maximum height of a PST can be 64. We can use a balanced version
 * of the priority search tree to optimize the tree height, but the balanced
 * tree proposed by McCreight is too complex and memory-hungry for our purpose.
 */

/*
 * [한국어] get_index - 노드에서 radix(start)와 heap(last) 인덱스를 추출
 *
 * @node:  대상 노드
 * @radix: [out] node->start 가 저장됨
 * @heap:  [out] node->last 가 저장됨
 *
 * start 와 last 는 사용자가 prio_tree_node 에 직접 기록하는 닫힌 구간의 양 끝이며,
 * PST 내부에서는 radix(start)/heap(last) 라는 이름으로 취급한다.
 * radix_index <= heap_index 가 항상 보장되어야 함 (호출자 책임).
 */
static void get_index(const struct prio_tree_node *node,
		      unsigned long *radix, unsigned long *heap)
{
	*radix = node->start;	/* [한국어] 구간 시작점 */
	*heap = node->last;	/* [한국어] 구간 종점 */
}

/* [한국어] index_bits_to_maxindex[i] = i+1 비트로 표현 가능한 최댓값 (2^(i+1) - 1).
 * prio_tree_init 에서 계산하여 prio_tree_maxindex 가 상수 시간에 참조한다.
 * 배열 크기는 BITS_PER_LONG(32 또는 64) 이며 마지막 원소는 ~0UL (bit-width 포화 방지). */
static unsigned long index_bits_to_maxindex[BITS_PER_LONG];

/*
 * [한국어] prio_tree_init - 생성자 함수: index_bits_to_maxindex[] 사전 계산
 *
 * fio_init 매크로( __attribute__((constructor)) )로 프로그램 시작 직후 자동 호출.
 * 이 초기화는 한 프로세스에서 한 번만 수행되며 이후 모든 PST 인스턴스가 공유.
 * 실행 컨텍스트: ld.so/loader 가 main() 이전에 .init_array 를 통해 호출.
 */
static void fio_init prio_tree_init(void)
{
	unsigned int i;

	for (i = 0; i < FIO_ARRAY_SIZE(index_bits_to_maxindex) - 1; i++)	/* [한국어] 마지막 원소 제외하고 "2^(i+1) - 1" 패턴 채움 */
		index_bits_to_maxindex[i] = (1UL << (i + 1)) - 1;
	index_bits_to_maxindex[FIO_ARRAY_SIZE(index_bits_to_maxindex) - 1] = ~0UL;	/* [한국어] 최상위: 전체 비트 포화 시 오버플로 방지용 sentinel */
}

/*
 * Maximum heap_index that can be stored in a PST with index_bits bits
 */
/*
 * [한국어] prio_tree_maxindex - index_bits 비트 PST 가 표현 가능한 최대 heap_index 반환
 *
 * 인라인으로 단일 테이블 조회(O(1)) 이며 bits >= 1 을 가정한다.
 */
static inline unsigned long prio_tree_maxindex(unsigned int bits)
{
	return index_bits_to_maxindex[bits - 1];	/* [한국어] 1-based bits 를 0-based 배열로 변환 */
}

/*
 * Extend a priority search tree so that it can store a node with heap_index
 * max_heap_index. In the worst case, this algorithm takes O((log n)^2).
 * However, this function is used rarely and the common case performance is
 * not bad.
 */
/*
 * [한국어] prio_tree_expand - max_heap_index 를 표현할 수 있도록 트리 bit 수를 늘림
 *
 * @root:            트리 루트 (index_bits 가 커질 수 있음)
 * @node:            새로 삽입될 노드 (확장 후 새 루트 체인의 일부로 사용됨)
 * @max_heap_index:  이 노드를 수용하기 위해 필요한 최대 heap_index
 * @return:          새 루트 체인에 포함된 node 포인터
 *
 * 내부 동작:
 *   - index_bits 를 한 단계씩 증가시키며, 기존 트리가 비어있지 않으면
 *     기존 루트를 떼어내 임시 체인(first..last)에 연결.
 *   - 모든 확장이 끝나면 임시 체인과 원래 트리 잔여부를 새 node 에 이어붙이고 node 를 루트로.
 * 최악 O((log n)^2): index_bits 가 여러 단계 증가하면서 각 단계에 prio_tree_remove 호출.
 */
static struct prio_tree_node *prio_tree_expand(struct prio_tree_root *root,
		struct prio_tree_node *node, unsigned long max_heap_index)
{
	struct prio_tree_node *first = NULL, *prev, *last = NULL;	/* [한국어] 확장 중 임시 체인의 시작/이전/끝 포인터 */

	if (max_heap_index > prio_tree_maxindex(root->index_bits))	/* [한국어] 현 bits 로 표현 불가 → 최소 1단계 확장 */
		root->index_bits++;

	while (max_heap_index > prio_tree_maxindex(root->index_bits)) {	/* [한국어] 충분해질 때까지 반복 증가 */
		root->index_bits++;

		if (prio_tree_empty(root))		/* [한국어] 트리가 비어있으면 별도 체인 작업 불필요 */
			continue;

		if (first == NULL) {
			first = root->prio_tree_node;	/* [한국어] 첫 떼어낸 기존 루트를 체인 시작점으로 */
			prio_tree_remove(root, root->prio_tree_node);	/* [한국어] 기존 루트를 트리에서 제거 */
			INIT_PRIO_TREE_NODE(first);		/* [한국어] 분리된 노드의 자식 링크 초기화 (자체 루프로 비움 표기) */
			last = first;
		} else {
			prev = last;			/* [한국어] 직전까지의 체인 끝을 기억 */
			last = root->prio_tree_node;	/* [한국어] 또 다른 기존 루트를 떼어내 체인 연장 */
			prio_tree_remove(root, root->prio_tree_node);
			INIT_PRIO_TREE_NODE(last);
			prev->left = last;		/* [한국어] 이전 체인 노드의 왼쪽 자식으로 새 노드 연결 */
			last->parent = prev;
		}
	}

	INIT_PRIO_TREE_NODE(node);			/* [한국어] 새 노드의 자식 링크 초기화 */

	if (first) {
		node->left = first;			/* [한국어] 임시 체인을 새 node 왼쪽 자식으로 연결 */
		first->parent = node;
	} else
		last = node;				/* [한국어] 체인이 없었으면 last=node 로 통일 */

	if (!prio_tree_empty(root)) {
		last->left = root->prio_tree_node;	/* [한국어] 트리 잔여부를 체인 끝에 이어붙임 */
		last->left->parent = last;
	}

	root->prio_tree_node = node;			/* [한국어] 새 루트 지정 */
	return node;
}

/*
 * Replace a prio_tree_node with a new node and return the old node
 */
/*
 * [한국어] prio_tree_replace - old 자리에 node 를 끼워넣고 old 를 분리해 반환
 *
 * @root: 트리 루트
 * @old:  교체 대상 기존 노드
 * @node: 새 노드 (자식 링크가 아직 초기화되지 않은 상태여야 함)
 * @return: old 포인터 (호출자가 재사용 또는 free)
 *
 * 부모 포인터, 좌/우 자식의 부모 포인터를 재연결하여 완전한 치환을 수행.
 * old 가 루트면 root->prio_tree_node 를 node 로 갱신.
 */
struct prio_tree_node *prio_tree_replace(struct prio_tree_root *root,
		struct prio_tree_node *old, struct prio_tree_node *node)
{
	INIT_PRIO_TREE_NODE(node);		/* [한국어] 새 노드 초기화 */

	if (prio_tree_root(old)) {		/* [한국어] old 가 루트 노드였는지 */
		assert(root->prio_tree_node == old);
		/*
		 * We can reduce root->index_bits here. However, it is complex
		 * and does not help much to improve performance (IMO).
		 */
		node->parent = node;		/* [한국어] 루트의 관용: 자기 자신을 부모로 지정 (detached 표식) */
		root->prio_tree_node = node;
	} else {
		node->parent = old->parent;	/* [한국어] 부모 상속 */
		if (old->parent->left == old)
			old->parent->left = node;	/* [한국어] 부모의 왼쪽 자리 */
		else
			old->parent->right = node;	/* [한국어] 부모의 오른쪽 자리 */
	}

	if (!prio_tree_left_empty(old)) {		/* [한국어] 왼쪽 자식 있으면 */
		node->left = old->left;
		old->left->parent = node;		/* [한국어] 자식의 부모를 갱신 */
	}

	if (!prio_tree_right_empty(old)) {		/* [한국어] 오른쪽 자식 있으면 */
		node->right = old->right;
		old->right->parent = node;
	}

	return old;				/* [한국어] 분리된 old 반환 */
}

/*
 * Insert a prio_tree_node @node into a radix priority search tree @root. The
 * algorithm typically takes O(log n) time where 'log n' is the number of bits
 * required to represent the maximum heap_index. In the worst case, the algo
 * can take O((log n)^2) - check prio_tree_expand.
 *
 * If a prior node with same radix_index and heap_index is already found in
 * the tree, then returns the address of the prior node. Otherwise, inserts
 * @node into the tree and returns @node.
 */
/*
 * [한국어] prio_tree_insert - PST 에 노드 삽입
 *
 * @root: 트리 루트
 * @node: 삽입할 노드 (start/last 사용자 지정, 링크는 아직 없음)
 * @return: 성공 시 node 자체. 동일 키 이미 존재 시 기존 노드 포인터 (중복 방지).
 *
 * 절차:
 *   1. 트리가 비었거나 heap_index 가 현재 bit 수로 표현 불가하면 prio_tree_expand 로 확장/삽입.
 *   2. 현재 bits 의 최상위 비트부터 mask 를 낮춰가며 radix_index (또는 size_flag 하에서 size) 비트에
 *      따라 좌/우로 진행.
 *   3. 경로 상에서 heap_index 가 더 큰 노드를 만나면 자리를 맞교환(최대 힙 유지): prio_tree_replace 사용.
 *   4. 리프 수준에서 빈 슬롯을 찾으면 연결하고 종료.
 *   5. mask 가 0 에 도달하면 size 로 재시작 (size_flag=1) — 복합 키의 두 번째 구성요소.
 *
 * 실행 컨텍스트: 상위 호출자의 락 보호 하에 실행. 본 함수는 내부적으로 락 없음.
 */
struct prio_tree_node *prio_tree_insert(struct prio_tree_root *root,
		struct prio_tree_node *node)
{
	struct prio_tree_node *cur, *res = node;			/* [한국어] cur=현재 순회 포인터, res=성공 시 반환 기본값 */
	unsigned long radix_index, heap_index;				/* [한국어] 삽입하려는 노드의 키 */
	unsigned long r_index, h_index, index, mask;			/* [한국어] 순회 중 cur 의 키, 임시 교환용 index, 비트 마스크 */
	int size_flag = 0;						/* [한국어] 0=radix 비트 사용, 1=size(heap-radix) 비트 사용 (두 단계) */

	get_index(node, &radix_index, &heap_index);			/* [한국어] 삽입 대상의 키 추출 */

	if (prio_tree_empty(root) ||
			heap_index > prio_tree_maxindex(root->index_bits))	/* [한국어] 확장 필요 조건 */
		return prio_tree_expand(root, node, heap_index);

	cur = root->prio_tree_node;					/* [한국어] 루트에서 탐색 시작 */
	mask = 1UL << (root->index_bits - 1);				/* [한국어] 최상위 비트부터 검사 */

	while (mask) {
		get_index(cur, &r_index, &h_index);			/* [한국어] 현재 노드 키 */

		if (r_index == radix_index && h_index == heap_index)	/* [한국어] 동일 키 → 중복 방지, 기존 노드 반환 */
			return cur;

                if (h_index < heap_index ||
		    (h_index == heap_index && r_index > radix_index)) {	/* [한국어] 힙 속성 위반 → 노드 교환 (상위에 heap 큰 값이 와야 함) */
			struct prio_tree_node *tmp = node;
			node = prio_tree_replace(root, cur, node);	/* [한국어] cur 자리에 node 를 넣고 cur 를 떼어냄 */
			cur = tmp;					/* [한국어] 삽입 계속: 원래 node 가 이제 cur 자리에 있고, cur 는 옛 node */
			/* swap indices */
			index = r_index;				/* [한국어] radix 교환 */
			r_index = radix_index;
			radix_index = index;
			index = h_index;				/* [한국어] heap 교환 */
			h_index = heap_index;
			heap_index = index;
		}

		if (size_flag)						/* [한국어] 두 번째 단계: size 비트 기반 분기 */
			index = heap_index - radix_index;
		else							/* [한국어] 첫 단계: radix 비트 기반 */
			index = radix_index;

		if (index & mask) {					/* [한국어] 해당 비트가 1 → 오른쪽으로 */
			if (prio_tree_right_empty(cur)) {		/* [한국어] 빈 슬롯이면 여기에 붙이고 종료 */
				INIT_PRIO_TREE_NODE(node);
				cur->right = node;
				node->parent = cur;
				return res;
			} else
				cur = cur->right;			/* [한국어] 이미 자식 있으면 하향 계속 */
		} else {						/* [한국어] 해당 비트가 0 → 왼쪽으로 */
			if (prio_tree_left_empty(cur)) {
				INIT_PRIO_TREE_NODE(node);
				cur->left = node;
				node->parent = cur;
				return res;
			} else
				cur = cur->left;
		}

		mask >>= 1;						/* [한국어] 다음 하위 비트로 */

		if (!mask) {						/* [한국어] radix 비트 소진 → size 비트로 두 번째 단계 재시작 */
			mask = 1UL << (BITS_PER_LONG - 1);
			size_flag = 1;
		}
	}
	/* Should not reach here */
	assert(0);							/* [한국어] 논리상 도달 불가. 도달 시 버그 — 디버그 빌드에서 중단 */
	return NULL;
}

/*
 * Remove a prio_tree_node @node from a radix priority search tree @root. The
 * algorithm takes O(log n) time where 'log n' is the number of bits required
 * to represent the maximum heap_index.
 */
/*
 * [한국어] prio_tree_remove - node 를 트리에서 제거
 *
 * @root: 트리 루트
 * @node: 제거할 노드
 *
 * 알고리즘:
 *   1. node 를 시작점으로, 좌/우 자식 중 heap_index 가 큰 쪽으로 "회전" 하며
 *      리프 쪽으로 내려간다(= node 의 자리에 자식을 끌어올림, 최대 힙 복구).
 *   2. 최종 도달한 cur 가 루트면 트리 전체 비움, 아니면 부모-자식 링크를
 *      "자기 자신을 가리키도록" 만들어 단절.
 *   3. 끌어올려진 자식들을 prio_tree_replace 로 연쇄 치환하며 node 까지 수렴.
 *
 * 결과: node 는 트리에서 분리되며 호출자가 추가 처리(할당 해제 등)를 해야 한다.
 */
void prio_tree_remove(struct prio_tree_root *root, struct prio_tree_node *node)
{
	struct prio_tree_node *cur;
	unsigned long r_index, h_index_right, h_index_left;

	cur = node;

	while (!prio_tree_left_empty(cur) || !prio_tree_right_empty(cur)) {	/* [한국어] 리프가 될 때까지 하향 */
		if (!prio_tree_left_empty(cur))
			get_index(cur->left, &r_index, &h_index_left);
		else {
			cur = cur->right;		/* [한국어] 왼쪽 없으면 오른쪽으로 */
			continue;
		}

		if (!prio_tree_right_empty(cur))
			get_index(cur->right, &r_index, &h_index_right);
		else {
			cur = cur->left;		/* [한국어] 오른쪽 없으면 왼쪽으로 */
			continue;
		}

		/* both h_index_left and h_index_right cannot be 0 */
		if (h_index_left >= h_index_right)	/* [한국어] 더 큰 heap 쪽을 선택 (최대 힙 속성 유지) */
			cur = cur->left;
		else
			cur = cur->right;
	}

	if (prio_tree_root(cur)) {			/* [한국어] cur 가 루트면 트리 비움 */
		assert(root->prio_tree_node == cur);
		INIT_PRIO_TREE_ROOT(root);
		return;
	}

	if (cur->parent->right == cur)			/* [한국어] 부모의 자식 링크를 자신으로(empty 표시) 돌림 */
		cur->parent->right = cur->parent;
	else
		cur->parent->left = cur->parent;

	while (cur != node)				/* [한국어] cur 를 부모 방향으로 치환하며 node 까지 수렴 */
		cur = prio_tree_replace(root, cur->parent, cur);
}

/*
 * Following functions help to enumerate all prio_tree_nodes in the tree that
 * overlap with the input interval X [radix_index, heap_index]. The enumeration
 * takes O(log n + m) time where 'log n' is the height of the tree (which is
 * proportional to # of bits required to represent the maximum heap_index) and
 * 'm' is the number of prio_tree_nodes that overlap the interval X.
 */

/*
 * [한국어] prio_tree_left - 반복자를 왼쪽 자식으로 이동 (쿼리 범위와 겹칠 때만)
 *
 * @iter:     반복자 상태
 * @r_index: [out] 이동한 노드의 radix_index
 * @h_index: [out] 이동한 노드의 heap_index
 * @return:   이동한 노드 또는 NULL (왼쪽이 비었거나 가지치기됨)
 *
 * 이동 시 iter->mask/size_level 도 함께 갱신되어 탐색 비트 상태를 유지한다.
 * iter->r_index <= h_index 로 왼쪽 서브트리에 겹침 가능성이 있는지 선검사.
 */
static struct prio_tree_node *prio_tree_left(struct prio_tree_iter *iter,
		unsigned long *r_index, unsigned long *h_index)
{
	if (prio_tree_left_empty(iter->cur))
		return NULL;					/* [한국어] 왼쪽 자식 없음 */

	get_index(iter->cur->left, r_index, h_index);		/* [한국어] 왼쪽 자식의 키 획득 */

	if (iter->r_index <= *h_index) {			/* [한국어] 쿼리 [q_r, q_h] 와 노드 [r, h] 의 r <= q_h 가 된 이후, q_r <= h 만 추가 확인하면 겹침 가능 */
		iter->cur = iter->cur->left;			/* [한국어] 실제 이동 */
		iter->mask >>= 1;
		if (iter->mask) {
			if (iter->size_level)
				iter->size_level++;		/* [한국어] size 단계 깊이 증가 */
		} else {
			if (iter->size_level) {			/* [한국어] radix+size 비트 양쪽 다 소진 — 리프 수준 */
				assert(prio_tree_left_empty(iter->cur));
				assert(prio_tree_right_empty(iter->cur));
				iter->size_level++;
				iter->mask = ULONG_MAX;		/* [한국어] mask 소진 표시 (prio_tree_parent 에서 복구) */
			} else {
				iter->size_level = 1;		/* [한국어] size 단계 진입 */
				iter->mask = 1UL << (BITS_PER_LONG - 1);	/* [한국어] size 단계의 최상위 비트부터 */
			}
		}
		return iter->cur;
	}

	return NULL;						/* [한국어] 겹침 가능성 없음 → 가지치기 */
}

/*
 * [한국어] prio_tree_right - 반복자를 오른쪽 자식으로 이동 (쿼리 범위와 겹칠 때만)
 *
 * 왼쪽 이동과 유사하지만, 오른쪽 서브트리에는 radix_index 비트가 1인 값만 들어있으므로
 * value | mask 로 "오른쪽 영역의 최소 heap_index 하한" 을 계산해 가지치기한다.
 */
static struct prio_tree_node *prio_tree_right(struct prio_tree_iter *iter,
		unsigned long *r_index, unsigned long *h_index)
{
	unsigned long value;

	if (prio_tree_right_empty(iter->cur))
		return NULL;

	if (iter->size_level)
		value = iter->value;			/* [한국어] size 단계에서는 value 그대로 사용 */
	else
		value = iter->value | iter->mask;	/* [한국어] radix 단계: 오른쪽으로 가면 이 비트가 1 로 세팅 */

	if (iter->h_index < value)
		return NULL;				/* [한국어] 쿼리 상한이 value 보다 작으면 오른쪽 서브트리 전체 무시 가능 */

	get_index(iter->cur->right, r_index, h_index);

	if (iter->r_index <= *h_index) {		/* [한국어] 겹침 가능성 */
		iter->cur = iter->cur->right;
		iter->mask >>= 1;
		iter->value = value;			/* [한국어] 현재 경로의 radix 비트 상태 반영 */
		if (iter->mask) {
			if (iter->size_level)
				iter->size_level++;
		} else {
			if (iter->size_level) {
				assert(prio_tree_left_empty(iter->cur));
				assert(prio_tree_right_empty(iter->cur));
				iter->size_level++;
				iter->mask = ULONG_MAX;
			} else {
				iter->size_level = 1;
				iter->mask = 1UL << (BITS_PER_LONG - 1);
			}
		}
		return iter->cur;
	}

	return NULL;
}

/*
 * [한국어] prio_tree_parent - 반복자를 부모로 이동 (상향)
 *
 * mask 와 size_level 을 원래대로 되돌린다. value 에서 자신이 세팅했던 비트를
 * XOR 로 클리어하여 경로 상태를 이전 수준으로 복원.
 */
static struct prio_tree_node *prio_tree_parent(struct prio_tree_iter *iter)
{
	iter->cur = iter->cur->parent;			/* [한국어] 부모로 이동 */
	if (iter->mask == ULONG_MAX)
		iter->mask = 1UL;			/* [한국어] 리프 끝에서 올라옴 */
	else if (iter->size_level == 1)
		iter->mask = 1UL;			/* [한국어] size 단계 탈출 직전 */
	else
		iter->mask <<= 1;			/* [한국어] 한 비트 복구 */
	if (iter->size_level)
		iter->size_level--;			/* [한국어] size 단계 깊이 감소 */
	if (!iter->size_level && (iter->value & iter->mask))
		iter->value ^= iter->mask;		/* [한국어] radix 단계에서 자신이 세팅한 비트 클리어 */
	return iter->cur;
}

/*
 * [한국어] overlap - 쿼리 [iter->r_index, iter->h_index] 와 노드 [r_index, h_index] 겹침 판정
 *
 * 두 닫힌 구간 [a, b] 와 [c, d] 는 a<=d && c<=b 이면 겹친다.
 */
static inline int overlap(struct prio_tree_iter *iter,
		unsigned long r_index, unsigned long h_index)
{
	return iter->h_index >= r_index && iter->r_index <= h_index;
}

/*
 * prio_tree_first:
 *
 * Get the first prio_tree_node that overlaps with the interval [radix_index,
 * heap_index]. Note that always radix_index <= heap_index. We do a pre-order
 * traversal of the tree.
 */
/*
 * [한국어] prio_tree_first - 쿼리 구간과 겹치는 "첫" 노드 반환 (전위 순회 시작)
 *
 * @iter: 반복자 (root, r_index, h_index 설정된 상태로 호출)
 * @return: 첫 겹침 노드 또는 NULL
 *
 * INIT_PRIO_TREE_ITER 로 내부 상태 초기화 후 루트부터 전위 순회.
 * 각 노드에서 overlap() 검사하여 해당되면 반환.
 */
static struct prio_tree_node *prio_tree_first(struct prio_tree_iter *iter)
{
	struct prio_tree_root *root;
	unsigned long r_index, h_index;

	INIT_PRIO_TREE_ITER(iter);				/* [한국어] iter->cur/mask/value/size_level 초기화 */

	root = iter->root;
	if (prio_tree_empty(root))
		return NULL;

	get_index(root->prio_tree_node, &r_index, &h_index);	/* [한국어] 루트의 키 */

	if (iter->r_index > h_index)
		return NULL;					/* [한국어] 쿼리 하한 > 최대 heap → 겹침 불가능 */

	iter->mask = 1UL << (root->index_bits - 1);		/* [한국어] 최상위 비트부터 */
	iter->cur = root->prio_tree_node;			/* [한국어] 루트에서 시작 */

	while (1) {
		if (overlap(iter, r_index, h_index))
			return iter->cur;			/* [한국어] 현재 노드가 겹치면 즉시 반환 */

		if (prio_tree_left(iter, &r_index, &h_index))	/* [한국어] 왼쪽 이동 성공 시 반복 */
			continue;

		if (prio_tree_right(iter, &r_index, &h_index))	/* [한국어] 오른쪽 이동 성공 시 반복 */
			continue;

		break;						/* [한국어] 양쪽 이동 실패 → 종료 */
	}
	return NULL;
}

/*
 * prio_tree_next:
 *
 * Get the next prio_tree_node that overlaps with the input interval in iter
 */
/*
 * [한국어] prio_tree_next - 쿼리 구간과 겹치는 다음 노드를 반환
 *
 * @iter: 반복자 (root, r_index, h_index 로 쿼리 범위 지정)
 * @return: 다음 겹치는 노드 (없으면 NULL)
 *
 * 전위 순회(pre-order) 로 트리를 탐색하며, 겹치는 노드만 반환한다.
 * iter->cur 이 NULL 이면 prio_tree_first() 로 시작한다.
 * 시간 복잡도: O(log n + m) (n=트리 크기, m=겹치는 노드 수).
 *
 * 알고리즘:
 *   1. 왼쪽 자식이 존재하고 겹칠 가능성이 있으면 왼쪽으로 내려가며 overlap 반환.
 *   2. 왼쪽 자식이 없거나 소진되면 오른쪽 방향을 시도하며 부모로 상향.
 *   3. 루트로 올라오면 NULL.
 *
 * 호출 체인: gfio graph.c → prio_tree_iter_init → [prio_tree_next].
 */
struct prio_tree_node *prio_tree_next(struct prio_tree_iter *iter)
{
	unsigned long r_index, h_index;

	if (iter->cur == NULL)
		return prio_tree_first(iter);			/* [한국어] 첫 호출 처리 */

repeat:
	while (prio_tree_left(iter, &r_index, &h_index))	/* [한국어] 가능한 만큼 왼쪽으로 내려감 */
		if (overlap(iter, r_index, h_index))
			return iter->cur;			/* [한국어] 겹치는 노드 즉시 반환 */

	while (!prio_tree_right(iter, &r_index, &h_index)) {	/* [한국어] 왼쪽 막히면 오른쪽 시도, 실패면 상향 */
	    	while (!prio_tree_root(iter->cur) &&
				iter->cur->parent->right == iter->cur)	/* [한국어] 오른쪽 자식 체인을 계속 올라감 (이미 방문) */
			prio_tree_parent(iter);

		if (prio_tree_root(iter->cur))
			return NULL;				/* [한국어] 루트까지 도달 → 순회 완료 */

		prio_tree_parent(iter);				/* [한국어] 왼쪽 자식이었던 위치 → 한 번 더 위로 */
	}

	if (overlap(iter, r_index, h_index))			/* [한국어] 오른쪽으로 이동한 노드가 겹치면 반환 */
		return iter->cur;

	goto repeat;						/* [한국어] 아니면 다시 왼쪽으로 내려가기 시도 */
}
