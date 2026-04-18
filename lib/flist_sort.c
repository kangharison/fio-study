/*
 * [한국어 설명] fio 연결 리스트 병합 정렬 (flist_sort.c)
 *
 * === 파일의 역할 ===
 * fio 의 이중 연결 리스트 API(struct flist_head) 위에서 O(n log n) 안정(stable)
 * 병합 정렬을 구현한다. Linux 커널의 lib/list_sort.c 를 거의 그대로 포팅한 코드로,
 * 바텀업(bottom-up) 방식의 병합 정렬을 사용한다: 입력 리스트를 하나씩 소비하며
 * "크기 2^i 의 정렬된 부분 리스트" 를 part[i] 슬롯에 유지하고, 같은 크기가 충돌할
 * 때마다 병합하여 한 단계 위 슬롯으로 올리는 방식이다. 최종적으로 남은 모든 슬롯을
 * 병합하고 이중 연결 구조(prev 링크)를 복원한다. "같은 키" 원소에 대해 a 를 먼저
 * 취하는 결정으로 안정성을 보장한다.
 *
 * MAX_LIST_LENGTH_BITS=20 은 2^20=1,048,576 원소까지 최적 처리를 보장 — 초과 시
 * 경고 후에도 동작은 하나 log_err 로 보고하고 일부 슬롯은 재사용된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 범용 리스트 정렬 유틸. fio 내부에서 I/O 히스토리, 스케줄 큐, 통계 집합 등 다양한
 * 연결 리스트 정렬에 호출된다. 비교 함수(cmp)는 호출자가 제공하며, priv 포인터로
 * 비교 컨텍스트(클라이언트 상태) 를 넘길 수 있어 재진입/공유 데이터 스레드 안전성은
 * cmp 의 특성에 따른다.
 *
 * === 타 모듈과의 연결 ===
 * - ../flist.h:  struct flist_head 정의, flist_empty/flist_first_entry 등 매크로.
 * - ../log.h:    log_err — MAX_LIST_LENGTH_BITS 초과 경고 출력.
 * - 호출자:      각종 fio 서브시스템 — 리스트 정렬이 필요한 곳.
 *
 * === 주요 함수/구조체 요약 ===
 * - flist_sort(priv, head, cmp): 공개 API — 이중 연결 리스트 head 의 원소를 cmp 로 정렬.
 *   결과는 동일한 head 를 재사용하는 제자리(in-place) 정렬. prev 링크 복원 포함.
 * - merge (static): null-terminated 두 정렬 리스트를 하나로 병합. 반환은 head 포인터(next 만 유지).
 * - merge_and_restore_back_links (static): 최종 병합 + prev 링크 복원을 한 번에 수행.
 *   별도 복원 패스보다 빠르며, cmp 를 동일 인자로 한 번 더 호출해 cond_resched() 같은
 *   클라이언트 훅이 주기적으로 실행될 기회를 제공.
 * 자체 구조체 없음 — flist.h 의 flist_head 사용.
 */
#include <stdio.h>              /* [한국어] 표준 I/O — log.h 경로와의 의존성 간접 제공 */
#include <string.h>             /* [한국어] memset — part[] 배열 초기화 */
#include "../flist.h"           /* [한국어] struct flist_head 정의와 flist_empty 매크로 */
#include "../log.h"             /* [한국어] log_err — 과도한 리스트 길이 경고 */

/* [한국어] 바텀업 병합에서 최대 이진 지수 — 슬롯 개수.
 * 2^20 = 1,048,576 원소까지 각 단계별 슬롯 하나로 처리 가능.
 * 이를 넘으면 slot 부족으로 병합이 정확히 계단형이 아닐 수 있어 경고 출력 */
#define MAX_LIST_LENGTH_BITS 20

/*
 * Returns a list organized in an intermediate format suited
 * to chaining of merge() calls: null-terminated, no reserved or
 * sentinel head node, "prev" links not maintained.
 */
/*
 * [한국어]
 * merge - 두 개의 정렬된 단방향(next 만 유지) 리스트를 하나로 병합.
 *
 * @priv: 비교 함수에 그대로 전달되는 클라이언트 컨텍스트.
 * @cmp:  (priv, a, b) → 음수: a 먼저, 양수: b 먼저, 0: 동일.
 * @a, @b: 각각 NULL-terminated, 정렬된 리스트의 첫 노드.
 * @return: 병합 결과의 첫 노드(NULL-terminated, prev 링크 유지 안 함).
 *
 * 안정성: cmp 반환 <= 0 일 때 a 를 먼저 꺼냄 — 동일 키에서 원래 순서 유지.
 *
 * 동작 단계:
 *   1) local head 구조체를 sentinel 로 두어 tail 포인터로 append.
 *   2) a, b 둘 다 비어있지 않은 동안 반복: 작거나 같은 쪽을 tail->next 에 연결.
 *   3) 한 쪽이 소진되면 남은 쪽 전체를 tail->next 에 붙임(?: 연산자).
 *   4) 반환: head.next (실제 병합 결과 첫 노드).
 *
 * 실행 컨텍스트: 정렬 보조 함수 — 호출자 flist_sort 의 재귀적 병합 단계에서 실행.
 *
 * 호출 체인: flist_sort → [merge]; merge 자체는 재귀 없음.
 */
static struct flist_head *merge(void *priv,
				int (*cmp)(void *priv, struct flist_head *a,
					struct flist_head *b),
				struct flist_head *a, struct flist_head *b)
{
	/* [한국어] 로컬 sentinel 헤드 + 말단 포인터. head 자체는 반환 전 즉시 버려짐 */
	struct flist_head head, *tail = &head;

	/* [한국어] 두 리스트 모두 비지 않은 동안 하나씩 꺼내 병합 */
	while (a && b) {
		/* if equal, take 'a' -- important for sort stability */
		/* [한국어] cmp<=0 이면 a 를 먼저 취하여 안정성 보장 */
		if ((*cmp)(priv, a, b) <= 0) {
			tail->next = a;     /* [한국어] 현재 말단 뒤에 a 노드 연결 */
			a = a->next;        /* [한국어] a 리스트의 다음 노드로 전진 */
		} else {
			tail->next = b;     /* [한국어] b 가 더 작음 — b 먼저 연결 */
			b = b->next;
		}
		tail = tail->next;        /* [한국어] 말단을 방금 연결한 노드로 이동 */
	}
	/* [한국어] 남은 쪽 전체를 꼬리에 붙임(a 또는 b 둘 중 하나는 NULL) */
	tail->next = a?:b;
	/* [한국어] sentinel 다음 노드부터가 실제 결과 시작 */
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
 * [한국어]
 * merge_and_restore_back_links - 최종 병합과 이중 연결(prev) 복원을 동시에 수행.
 *
 * @priv: cmp 에 전달할 클라이언트 컨텍스트.
 * @cmp:  비교 함수.
 * @head: 최종 결과의 소유 헤드(사용자가 넘긴 원래 head, 원형 이중 연결 리스트로 복원).
 * @a, @b: 병합할 두 정렬 리스트(단방향 상태).
 *
 * 왜 이 함수가 별도 존재하는가?
 * - 매 병합마다 prev 링크를 복원하면 로직이 복잡해지고 성능 저하.
 * - 그렇다고 최종적으로 별도 prev-복원 패스를 돌리면 전체 리스트를 한 번 더 순회.
 * - 절충으로 "최종 병합 루프에서 prev 를 함께 설정" 하는 방식을 택함.
 *   코드 중복이 발생하지만(merge 와 유사) 최종 경로만 이 방식을 써 성능 우위.
 *
 * 또한 말단 "a 나 b 남은 쪽 전체" 를 prev 복원 루프에서 cmp(tail->next, tail->next)
 * 를 의도적으로 호출하는데, 이는 Linux 커널 원본의 설계:
 * 클라이언트 cmp 가 cond_resched()/cpu_relax() 같은 훅을 쓸 경우 "긴 리스트 꼬리에서도"
 * 그 훅이 주기적으로 호출되도록 보장한다(비교 결과 자체는 버려짐).
 *
 * 동작 단계:
 *   1) tail=head 에서 시작. a,b 둘 다 있는 동안 cmp 로 비교 후 tail->next=선택,
 *      선택된 노드의 prev=tail 설정(이중 링크 복원), 선택된 쪽 포인터 전진.
 *   2) 한 쪽 소진 시 남은 쪽을 꼬리에 붙임.
 *   3) 꼬리에 남은 체인에서 prev 를 이어 가며 cmp 훅 호출.
 *   4) 마지막 노드의 next 를 head 로, head->prev 를 마지막 노드로 설정해 원형 구조 복원.
 */
static void merge_and_restore_back_links(void *priv,
				int (*cmp)(void *priv, struct flist_head *a,
					struct flist_head *b),
				struct flist_head *head,
				struct flist_head *a, struct flist_head *b)
{
	/* [한국어] 말단 포인터 — head 자체를 sentinel 로 사용 */
	struct flist_head *tail = head;

	/* [한국어] 주 병합 루프 — merge() 와 유사하나 prev 링크도 동시에 설정 */
	while (a && b) {
		/* if equal, take 'a' -- important for sort stability */
		if ((*cmp)(priv, a, b) <= 0) {
			tail->next = a;     /* [한국어] a 를 현재 말단 뒤에 연결 */
			a->prev = tail;     /* [한국어] a 의 prev 를 현재 말단으로 (이중 연결 복원) */
			a = a->next;        /* [한국어] a 다음 노드로 */
		} else {
			tail->next = b;
			b->prev = tail;
			b = b->next;
		}
		tail = tail->next;
	}
	/* [한국어] 남은 리스트를 꼬리에 붙임(단방향 상태) — 아래 루프에서 prev 복원 */
	tail->next = a ? : b;

	/* [한국어] 남은 꼬리 체인의 prev 링크 복원 + cmp 훅 호출(클라이언트 cond_resched 기회).
	 * tail->next 는 아직 prev 가 설정 안 된 상태라 복원 필요 */
	do {
		/*
		 * In worst cases this loop may run many iterations.
		 * Continue callbacks to the client even though no
		 * element comparison is needed, so the client's cmp()
		 * routine can invoke cond_resched() periodically.
		 */
		/* [한국어] 동일 인자로 cmp 호출 — 결과 무시. 클라이언트의 주기적 훅 호출 기회만 제공 */
		(*cmp)(priv, tail->next, tail->next);

		tail->next->prev = tail;   /* [한국어] prev 링크 복원 */
		tail = tail->next;         /* [한국어] 말단 전진 */
	} while (tail->next);          /* [한국어] 끝(NULL) 에 도달하면 종료 */

	/* [한국어] 원형 이중 연결 복원 — 마지막 노드의 next=head, head->prev=마지막 노드 */
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
 * [한국어]
 * flist_sort - fio flist_head 리스트를 O(n log n) 안정 병합 정렬.
 *
 * @priv: cmp 에 그대로 전달되는 클라이언트 컨텍스트 포인터.
 * @head: 정렬할 원형 이중 연결 리스트의 헤드. 비어 있으면 즉시 반환.
 * @cmp:  비교 함수 — 음수: a 먼저, 양수: b 먼저, 0: 동일(안정 유지).
 *
 * 바텀업 병합의 데이터 구조:
 *   part[0..MAX_LIST_LENGTH_BITS]: part[i] 는 크기 2^i 의 정렬된 부분 리스트(또는 NULL).
 *   마지막 슬롯(part[MAX_LIST_LENGTH_BITS]) 은 sentinel 로 남겨 둔다.
 *
 * 동작 단계:
 *   1) 리스트 비어있으면 반환.
 *   2) part[] 를 0 으로 초기화.
 *   3) 원형 구조를 임시로 단방향 NULL-terminated 로 변환(head->prev->next=NULL).
 *   4) 리스트 첫 노드부터 하나씩 cur 로 꺼내며:
 *      - part[0] 부터 비어있는 슬롯을 찾을 때까지 병합하며 위로 올림(캐리 전파와 유사).
 *      - 슬롯 lev 가 MAX_LIST_LENGTH_BITS 이상이면 log_err 로 경고 후 slot 재사용.
 *      - 발견한 첫 빈 슬롯에 cur 저장.
 *   5) 모든 원소 처리 후, 하위 슬롯부터 차례로 병합하여 하나의 리스트로 통합.
 *   6) 마지막 병합은 merge_and_restore_back_links 로 prev 링크 복원 + 원형 구조 복원.
 *
 * 실행 컨텍스트: 호출자 스레드. MT 안전성은 cmp/priv 에 따름.
 *
 * 호출 체인: 다양한 fio 서브시스템 → [flist_sort] → merge / merge_and_restore_back_links.
 *
 * 에러 처리: MAX_LIST_LENGTH_BITS 초과는 치명적 실패가 아니며, log_err 경고 후
 *           슬롯 재사용(결과 정확성은 유지되나 최적 성능에서 이탈).
 */
void flist_sort(void *priv, struct flist_head *head,
		int (*cmp)(void *priv, struct flist_head *a,
			struct flist_head *b))
{
	/* [한국어] part[i] 는 크기 2^i 의 정렬된 부분 리스트. 마지막 슬롯은 sentinel */
	struct flist_head *part[MAX_LIST_LENGTH_BITS+1]; /* sorted partial lists
						-- last slot is a sentinel */
	/* [한국어] lev: 현재 사용 중인 슬롯 인덱스. max_lev: 실제 도달한 최대 슬롯 */
	int lev;  /* index into part[] */
	int max_lev = 0;
	/* [한국어] 아직 처리되지 않은 원소 리스트의 현재 포인터 */
	struct flist_head *list;

	/* [한국어] 빈 리스트는 정렬할 것 없음 */
	if (flist_empty(head))
		return;

	/* [한국어] 모든 슬롯 NULL 로 초기화 — 슬롯 비어있음을 의미 */
	memset(part, 0, sizeof(part));

	/* [한국어] 원형 구조를 NULL-terminated 로 변환.
	 * 마지막 노드의 next 를 NULL 로 만들어 아래 루프의 종료 조건으로 사용 */
	head->prev->next = NULL;
	/* [한국어] 첫 원소 포인터(head->next) 로 순회 시작 */
	list = head->next;

	/* [한국어] 리스트 원소를 하나씩 꺼내며 part[] 슬롯에 적재 */
	while (list) {
		/* [한국어] 현재 노드 추출 후 list 를 다음으로 전진 */
		struct flist_head *cur = list;
		list = list->next;
		cur->next = NULL;           /* [한국어] cur 을 단독 노드(크기 1 리스트) 로 분리 */

		/* [한국어] 하위 슬롯부터 보면서 비지 않은 슬롯이 있으면 병합하여 크기 2 배로 올림.
		 * 바이너리 카운터의 캐리 전파와 동일 구조 */
		for (lev = 0; part[lev]; lev++) {
			cur = merge(priv, cmp, part[lev], cur);
			part[lev] = NULL;   /* [한국어] 병합 소비 후 슬롯 비움 */
		}
		/* [한국어] 최대 슬롯 갱신 + 슬롯 오버플로우 체크 */
		if (lev > max_lev) {
			if (lev >= MAX_LIST_LENGTH_BITS) {
				log_err("fio: list passed to"
					" list_sort() too long for"
					" efficiency\n");
				lev--;          /* [한국어] 최상위 슬롯 재사용(결과 정확도는 유지) */
			}
			max_lev = lev;
		}
		/* [한국어] 찾은 첫 빈 슬롯에 cur 적재 — 크기 2^lev */
		part[lev] = cur;
	}

	/* [한국어] 남은 하위 슬롯들을 차례로 병합하여 하나의 리스트로 통합.
	 * list 는 최상위 슬롯을 제외한 나머지를 계속 병합한 결과 */
	for (lev = 0; lev < max_lev; lev++)
		if (part[lev])
			list = merge(priv, cmp, part[lev], list);

	/* [한국어] 최상위 슬롯 part[max_lev] 과 list 를 최종 병합하며 prev/원형 구조 복원 */
	merge_and_restore_back_links(priv, cmp, head, part[max_lev], list);
}
