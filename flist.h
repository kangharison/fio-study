/*
 * [한국어] flist.h - fio 이중 연결 리스트 구현 (Linux 커널 list.h 기반)
 *
 * Linux 커널의 list.h를 fio 용도로 가져온 이중 연결 리스트이다.
 * 특징:
 *   - 침투적(intrusive) 리스트: 구조체 내에 flist_head를 임베딩하여 사용
 *   - container_of 매크로로 리스트 노드에서 부모 구조체 포인터를 역추적
 *   - 삽입, 삭제, 결합(splice), 순회 등 기본 연산 제공
 *   - flist_sort()로 병합 정렬 지원
 
 * === 파일의 역할 ===
 * Linux 커널 list.h 기반 이중 연결 리스트 구현. 침투적(intrusive) 리스트.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio 전체에서 가장 기본적인 자료구조. 거의 모든 모듈에서 사용.
 *
 * === 타 모듈과의 연결 ===
 * - fio 전체: io_u, fio_file, 워크큐 등 모든 리스트 관리
 *
 * === 주요 함수/구조체 요약 ===
 * - struct flist_head: 리스트 노드 (prev, next)
 * - flist_add/del/for_each: 삽입/삭제/순회
 * - container_of: 노드 → 부모 구조체 역추적
 */
#ifndef _LINUX_FLIST_H
#define _LINUX_FLIST_H

#include <stdlib.h>
#include <stddef.h>

/*
 * [한국어] container_of - 멤버 포인터로부터 부모 구조체의 포인터를 구함
 *
 * @ptr:    멤버의 포인터
 * @type:   부모 구조체의 타입
 * @member: 부모 구조체 내 해당 멤버의 이름
 *
 * 예: container_of(list_ptr, struct my_struct, list_member)
 *     -> list_ptr가 가리키는 flist_head를 포함하는 my_struct의 포인터 반환
 */
#define container_of(ptr, type, member)  ({			\
	const __typeof__( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})

/*
 * Simple doubly linked list implementation.
 *
 * Some of the internal functions ("__xxx") are useful when
 * manipulating whole lists rather than single entries, as
 * sometimes we already know the next/prev entries and we can
 * generate better code by using them directly rather than
 * using the generic single-entry routines.
 */

/* [한국어] 이중 연결 리스트의 노드/헤드 구조체 */
struct flist_head {
	struct flist_head *next, *prev; /* 다음/이전 노드 포인터 */
};

/* [한국어] 정적 초기화: 자기 자신을 가리키는 빈 리스트 */
#define FLIST_HEAD_INIT(name) { &(name), &(name) }

/* [한국어] 정적 리스트 헤드 선언 및 초기화 */
#define FLIST_HEAD(name) \
	struct flist_head name = FLIST_HEAD_INIT(name)

/* [한국어] 동적 리스트 헤드 초기화: next와 prev를 자기 자신으로 설정 */
#define INIT_FLIST_HEAD(ptr) do { \
	(ptr)->next = (ptr); (ptr)->prev = (ptr); \
} while (0)

/*
 * Insert a new entry between two known consecutive entries.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
/* [한국어] 내부 함수: 두 연속 노드 사이에 새 노드를 삽입 */
static inline void __flist_add(struct flist_head *new_entry,
			       struct flist_head *prev,
			       struct flist_head *next)
{
	next->prev = new_entry;
	new_entry->next = next;
	new_entry->prev = prev;
	prev->next = new_entry;
}

/**
 * flist_add - add a new entry
 * @new_entry: new entry to be added
 * @head: list head to add it after
 *
 * Insert a new entry after the specified head.
 * This is good for implementing stacks.
 */
/* [한국어] 리스트 헤드 바로 뒤에 새 노드 삽입 (스택 구현에 적합) */
static inline void flist_add(struct flist_head *new_entry,
                             struct flist_head *head)
{
	__flist_add(new_entry, head, head->next);
}

/* [한국어] 리스트 끝(헤드 바로 앞)에 새 노드 삽입 (큐 구현에 적합) */
static inline void flist_add_tail(struct flist_head *new_entry,
				  struct flist_head *head)
{
	__flist_add(new_entry, head->prev, head);
}

/*
 * Delete a list entry by making the prev/next entries
 * point to each other.
 *
 * This is only for internal list manipulation where we know
 * the prev/next entries already!
 */
/* [한국어] 내부 함수: prev와 next를 서로 연결하여 중간 노드를 제거 */
static inline void __flist_del(struct flist_head *prev,
			       struct flist_head * next)
{
	next->prev = prev;
	prev->next = next;
}

/**
 * flist_del - deletes entry from list.
 * @entry: the element to delete from the list.
 * Note: flist_empty on entry does not return true after this, the entry is
 * in an undefined state.
 */
/* [한국어] 리스트에서 노드를 삭제하고 포인터를 NULL로 설정 */
static inline void flist_del(struct flist_head *entry)
{
	__flist_del(entry->prev, entry->next);
	entry->next = NULL;
	entry->prev = NULL;
}

/**
 * flist_del_init - deletes entry from list and reinitialize it.
 * @entry: the element to delete from the list.
 */
/* [한국어] 리스트에서 노드를 삭제하고 빈 리스트로 재초기화 */
static inline void flist_del_init(struct flist_head *entry)
{
	__flist_del(entry->prev, entry->next);
	INIT_FLIST_HEAD(entry);
}

/**
 * flist_empty - tests whether a list is empty
 * @head: the list to test.
 */
/* [한국어] 리스트가 비어있는지 확인 (head->next == head이면 비어있음) */
static inline int flist_empty(const struct flist_head *head)
{
	return head->next == head;
}

/* [한국어] 내부 함수: list의 모든 노드를 prev와 next 사이에 결합(splice) */
static inline void __flist_splice(const struct flist_head *list,
				  struct flist_head *prev,
				  struct flist_head *next)
{
	struct flist_head *first = list->next;
	struct flist_head *last = list->prev;

	first->prev = prev;
	prev->next = first;

	last->next = next;
	next->prev = last;
}

/* [한국어] list의 모든 노드를 head 뒤에 결합 */
static inline void flist_splice(const struct flist_head *list,
				struct flist_head *head)
{
	if (!flist_empty(list))
		__flist_splice(list, head, head->next);
}

/* [한국어] list의 모든 노드를 head 앞(끝)에 결합 */
static inline void flist_splice_tail(struct flist_head *list,
				     struct flist_head *head)
{
	if (!flist_empty(list))
		__flist_splice(list, head->prev, head);
}

/* [한국어] list의 모든 노드를 head 앞(끝)에 결합하고, 원본 list를 재초기화 */
static inline void flist_splice_tail_init(struct flist_head *list,
					  struct flist_head *head)
{
	if (!flist_empty(list)) {
		__flist_splice(list, head->prev, head);
		INIT_FLIST_HEAD(list);
	}
}

/* [한국어] list의 모든 노드를 head 뒤에 결합하고, 원본 list를 재초기화 */
static inline void flist_splice_init(struct flist_head *list,
				    struct flist_head *head)
{
	if (!flist_empty(list)) {
		__flist_splice(list, head, head->next);
		INIT_FLIST_HEAD(list);
	}
}

/**
 * flist_entry - get the struct for this entry
 * @ptr:	the &struct flist_head pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the flist_struct within the struct.
 */
/* [한국어] 리스트 노드 포인터로부터 부모 구조체 포인터를 구함 */
#define flist_entry(ptr, type, member) \
	container_of(ptr, type, member)

/* [한국어] 리스트의 첫 번째 항목의 부모 구조체 포인터를 구함 */
#define flist_first_entry(ptr, type, member) \
	flist_entry((ptr)->next, type, member)

/* [한국어] 리스트의 마지막 항목의 부모 구조체 포인터를 구함 */
#define flist_last_entry(ptr, type, member) \
	flist_entry((ptr)->prev, type, member)

/**
 * flist_for_each	-	iterate over a list
 * @pos:	the &struct flist_head to use as a loop counter.
 * @head:	the head for your list.
 */
/* [한국어] 리스트를 순방향으로 순회 (순회 중 삭제 금지) */
#define flist_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)

/**
 * flist_for_each_safe	-	iterate over a list safe against removal of list entry
 * @pos:	the &struct flist_head to use as a loop counter.
 * @n:		another &struct flist_head to use as temporary storage
 * @head:	the head for your list.
 */
/* [한국어] 리스트를 순방향으로 순회 (순회 중 현재 노드 삭제 안전) */
#define flist_for_each_safe(pos, n, head) \
	for (pos = (head)->next, n = pos->next; pos != (head); \
		pos = n, n = pos->next)

/* [한국어] 리스트 정렬 (병합 정렬, 비교 함수와 사용자 데이터를 인자로 받음) */
extern void flist_sort(void *priv, struct flist_head *head,
	int (*cmp)(void *priv, struct flist_head *a, struct flist_head *b));

#endif
