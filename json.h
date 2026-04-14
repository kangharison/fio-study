/*
 * [한국어] json.h - fio JSON 출력 라이브러리 헤더
 *
 * fio 통계 결과를 JSON 형식으로 출력하기 위한 자료구조와 API를 정의한다.
 * 주요 구성:
 *   - json_value   : JSON 값 (정수, 실수, 문자열, 객체, 배열)
 *   - json_object  : JSON 객체 (키-값 쌍의 집합)
 *   - json_array   : JSON 배열 (값의 순서열)
 *   - json_pair    : JSON 키-값 쌍
 *   - 인라인 헬퍼 함수들: 타입별 값 추가를 편리하게 수행
 
 * === 파일의 역할 ===
 * fio JSON 출력을 위한 자료구조(json_value, json_object, json_array)와 API를 정의.
 *
 * === 전체 아키텍처에서의 위치 ===
 * json.c와 짝을 이루는 헤더. stat.c에서 JSON 출력 시 참조.
 *
 * === 타 모듈과의 연결 ===
 * - json.c: 이 헤더의 함수 구현
 * - stat.c: JSON 형식 통계 출력
 *
 * === 주요 함수/구조체 요약 ===
 * - struct json_value/object/array/pair: JSON 자료구조
 * - json_create_*/json_print_*: 생성/출력 API
 */
#ifndef __JSON__H
#define __JSON__H

#include "lib/output_buffer.h"

/* [한국어] JSON 값 타입 상수 */
#define JSON_TYPE_STRING 0
#define JSON_TYPE_INTEGER 1
#define JSON_TYPE_FLOAT 2
#define JSON_TYPE_OBJECT 3
#define JSON_TYPE_ARRAY 4

/* [한국어] JSON 값의 부모 타입 상수 (값이 pair 소속인지 array 소속인지 구분) */
#define JSON_PARENT_TYPE_PAIR 0
#define JSON_PARENT_TYPE_ARRAY 1

/* [한국어] json_value - JSON 값을 나타내는 구조체.
 *          type에 따라 union으로 실제 데이터를 저장한다.
 *          parent_type/parent_pair/parent_array로 트리 구조의 부모를 추적한다. */
struct json_value {
	int type;                       /* 값의 타입 (JSON_TYPE_* 상수) */
	union {
		long long integer_number;   /* 정수 값 */
		double float_number;        /* 실수 값 */
		char *string;               /* 문자열 값 (이스케이프 처리됨) */
		struct json_object *object; /* 객체 값 */
		struct json_array *array;   /* 배열 값 */
	};
	int parent_type;                /* 부모 타입 (JSON_PARENT_TYPE_* 상수) */
	union {
		struct json_pair *parent_pair;   /* 부모가 pair인 경우 */
		struct json_array *parent_array; /* 부모가 array인 경우 */
	};
};

/* [한국어] json_array - JSON 배열 구조체.
 *          동적 배열로 json_value 포인터들을 관리한다. */
struct json_array {
	struct json_value **values;  /* 값 포인터 배열 */
	int value_cnt;               /* 값 개수 */
	struct json_value *parent;   /* 부모 json_value */
};

/* [한국어] json_object - JSON 객체 구조체.
 *          동적 배열로 json_pair 포인터들을 관리한다. */
struct json_object {
	struct json_pair **pairs;    /* 키-값 쌍 포인터 배열 */
	int pair_cnt;                /* 쌍 개수 */
	struct json_value *parent;   /* 부모 json_value */
};

/* [한국어] json_pair - JSON 키-값 쌍 구조체 */
struct json_pair {
	char *name;                  /* 키 문자열 */
	struct json_value *value;    /* 값 */
	struct json_object *parent;  /* 소속 객체 */
};

/* [한국어] JSON 객체/배열 생성 및 해제 함수 */
struct json_object *json_create_object(void);
struct json_array *json_create_array(void);

void json_free_object(struct json_object *obj);

/* [한국어] 타입에 따라 객체에 값을 추가하는 범용 함수 */
int json_object_add_value_type(struct json_object *obj, const char *name,
			       const struct json_value *val);

/* [한국어] json_object_add_value_int - 객체에 정수 값 추가 */
static inline int json_object_add_value_int(struct json_object *obj,
					    const char *name, long long val)
{
	struct json_value arg = {
		.type = JSON_TYPE_INTEGER,
	};

	arg.integer_number = val;
	return json_object_add_value_type(obj, name, &arg);
}

/* [한국어] json_object_add_value_float - 객체에 실수 값 추가 */
static inline int json_object_add_value_float(struct json_object *obj,
					      const char *name, double val)
{
	struct json_value arg = {
		.type = JSON_TYPE_FLOAT,
	};

	arg.float_number = val;
	return json_object_add_value_type(obj, name, &arg);
}

/* [한국어] json_object_add_value_string - 객체에 문자열 값 추가.
 *          val이 NULL이면 빈 문자열("")로 대체한다. */
static inline int json_object_add_value_string(struct json_object *obj,
					       const char *name,
					       const char *val)
{
	struct json_value arg = {
		.type = JSON_TYPE_STRING,
	};
	union {
		const char *a;
		char *b;
	} string;

	string.a = val ? val : "";
	arg.string = string.b;
	return json_object_add_value_type(obj, name, &arg);
}

/* [한국어] json_object_add_value_object - 객체에 중첩 객체 값 추가 */
static inline int json_object_add_value_object(struct json_object *obj,
					       const char *name,
					       struct json_object *val)
{
	struct json_value arg = {
		.type = JSON_TYPE_OBJECT,
	};

	arg.object = val;
	return json_object_add_value_type(obj, name, &arg);
}

/* [한국어] json_object_add_value_array - 객체에 배열 값 추가 */
static inline int json_object_add_value_array(struct json_object *obj,
					      const char *name,
					      struct json_array *val)
{
	struct json_value arg = {
		.type = JSON_TYPE_ARRAY,
	};

	arg.array = val;
	return json_object_add_value_type(obj, name, &arg);
}

/* [한국어] 타입에 따라 배열에 값을 추가하는 범용 함수 */
int json_array_add_value_type(struct json_array *array,
			      const struct json_value *val);

/* [한국어] json_array_add_value_int - 배열에 정수 값 추가 */
static inline int json_array_add_value_int(struct json_array *obj,
					   long long val)
{
	struct json_value arg = {
		.type = JSON_TYPE_INTEGER,
	};

	arg.integer_number = val;
	return json_array_add_value_type(obj, &arg);
}

/* [한국어] json_array_add_value_float - 배열에 실수 값 추가 */
static inline int json_array_add_value_float(struct json_array *obj,
					     double val)
{
	struct json_value arg = {
		.type = JSON_TYPE_FLOAT,
	};

	arg.float_number = val;
	return json_array_add_value_type(obj, &arg);
}

/* [한국어] json_array_add_value_string - 배열에 문자열 값 추가 */
static inline int json_array_add_value_string(struct json_array *obj,
					      const char *val)
{
	struct json_value arg = {
		.type = JSON_TYPE_STRING,
	};

	arg.string = (char *)val;
	return json_array_add_value_type(obj, &arg);
}

/* [한국어] json_array_add_value_object - 배열에 객체 값 추가 */
static inline int json_array_add_value_object(struct json_array *obj,
					      struct json_object *val)
{
	struct json_value arg = {
		.type = JSON_TYPE_OBJECT,
	};

	arg.object = val;
	return json_array_add_value_type(obj, &arg);
}

/* [한국어] json_array_add_value_array - 배열에 중첩 배열 값 추가 */
static inline int json_array_add_value_array(struct json_array *obj,
					     struct json_array *val)
{
	struct json_value arg = {
		.type = JSON_TYPE_ARRAY,
	};

	arg.array = val;
	return json_array_add_value_type(obj, &arg);
}

/* [한국어] 배열의 마지막 값에서 object를 꺼내는 매크로 */
#define json_array_last_value_object(obj) \
	(obj->values[obj->value_cnt - 1]->object)

/* [한국어] JSON 객체를 포맷팅하여 출력 버퍼에 기록 */
void json_print_object(struct json_object *obj, struct buf_output *out);
#endif
