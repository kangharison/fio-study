/*
 * [한국어 설명] 블룸 필터 헤더 (bloom.h)
 *
 * === 파일의 역할 ===
 * 블룸 필터 자료구조의 공개 인터페이스를 정의한다. bloom_new로 생성하고,
 * bloom_set으로 원소를 추가하며, bloom_string으로 문자열 멤버십을 검사한다.
 *
 * === fio에서의 사용 ===
 * fio의 중복 제거 감지 기능에서 데이터 블록의 존재 여부를 빠르게 확인하기 위해
 * 이 헤더를 포함하여 블룸 필터 API를 사용한다.
 */
#ifndef FIO_BLOOM_H
#define FIO_BLOOM_H

#include <inttypes.h>
#include "../lib/types.h"

struct bloom;

struct bloom *bloom_new(uint64_t entries);
void bloom_free(struct bloom *b);
bool bloom_set(struct bloom *b, uint32_t *data, unsigned int nwords);
bool bloom_string(struct bloom *b, const char *data, unsigned int len, bool);

#endif
