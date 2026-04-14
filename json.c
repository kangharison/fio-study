/*
 * [한국어] json.c - fio JSON 출력 라이브러리 구현
 *
 * fio 통계 결과를 JSON 형식으로 출력하는 기능을 구현한다.
 * 주요 기능:
 *   1) json_create_*()        - JSON 객체/배열/값/쌍 생성
 *   2) json_free_*()          - JSON 트리 메모리 해제 (재귀적)
 *   3) json_object_add_*()    - 객체에 키-값 쌍 추가
 *   4) json_array_add_*()     - 배열에 값 추가
 *   5) json_print_*()         - JSON 트리를 문자열로 출력 (들여쓰기 포함)
 
 * === 파일의 역할 ===
 * fio 통계 결과를 JSON 형식으로 출력하는 기능을 구현한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * stat.c에서 JSON 출력 시 이 파일의 함수를 호출한다.
 *
 * === 타 모듈과의 연결 ===
 * - stat.c: JSON 형식 통계 출력 시 사용
 * - json.h: API 및 자료구조 선언
 *
 * === 주요 함수/구조체 요약 ===
 * - json_create_object/array/value(): JSON 노드 생성
 * - json_object_add_value_*(): 객체에 값 추가
 * - json_print_object(): JSON 트리 출력
 */

/* 표준 라이브러리 헤더 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include "json.h"
#include "log.h"

/* [한국어] json_create_object - 빈 JSON 객체를 생성하여 반환 */
struct json_object *json_create_object(void)
{
	return calloc(1, sizeof(struct json_object));
}

/* [한국어] json_create_array - 빈 JSON 배열을 생성하여 반환 */
struct json_array *json_create_array(void)
{
	return calloc(1, sizeof(struct json_array));
}

/* [한국어] json_create_pair - 이름과 값으로 JSON 키-값 쌍을 생성.
 *          값의 부모 타입을 PAIR로 설정한다. */
static struct json_pair *json_create_pair(const char *name, struct json_value *value)
{
	struct json_pair *pair = malloc(sizeof(struct json_pair));
	if (pair) {
		pair->name = strdup(name);
		pair->value = value;

		value->parent_type = JSON_PARENT_TYPE_PAIR;
		value->parent_pair = pair;
	}
	return pair;
}

/* [한국어] json_create_value_int - 정수 타입의 JSON 값 생성 */
static struct json_value *json_create_value_int(long long number)
{
	struct json_value *value = malloc(sizeof(struct json_value));

	if (value) {
		value->type = JSON_TYPE_INTEGER;
		value->integer_number = number;
	}
	return value;
}

/* [한국어] json_create_value_float - 실수 타입의 JSON 값 생성 */
static struct json_value *json_create_value_float(double number)
{
	struct json_value *value = malloc(sizeof(struct json_value));

	if (value) {
		value->type = JSON_TYPE_FLOAT;
		value->float_number = number;
	}
	return value;
}

/* [한국어] strdup_escape - 문자열을 복제하면서 '\' 와 '"' 문자를 이스케이프 처리.
 *          JSON 표준에 맞게 특수 문자 앞에 백슬래시를 추가한다. */
static char *strdup_escape(const char *str)
{
	const char *input = str;
	char *p, *ret;
	int escapes;

	escapes = 0;
	while ((input = strpbrk(input, "\\\"")) != NULL) {
		escapes++;
		input++;
	}

	p = ret = malloc(strlen(str) + escapes + 1);
	while (*str) {
		if (*str == '\\' || *str == '\"')
			*p++ = '\\';
		*p++ = *str++;
	}
	*p = '\0';

	return ret;
}

/*
 * Valid JSON strings must escape '"' and '/' with a preceding '/'
 */
/* [한국어] json_create_value_string - 문자열 타입의 JSON 값 생성.
 *          문자열은 strdup_escape()로 이스케이프 처리된 복사본이 저장된다. */
static struct json_value *json_create_value_string(const char *str)
{
	struct json_value *value = malloc(sizeof(struct json_value));

	if (value) {
		value->type = JSON_TYPE_STRING;
		value->string = strdup_escape(str);
		if (!value->string) {
			free(value);
			value = NULL;
		}
	}
	return value;
}

/* [한국어] json_create_value_object - 객체 타입의 JSON 값 생성.
 *          객체의 parent를 이 값으로 설정하여 트리 관계를 형성한다. */
static struct json_value *json_create_value_object(struct json_object *obj)
{
	struct json_value *value = malloc(sizeof(struct json_value));

	if (value) {
		value->type = JSON_TYPE_OBJECT;
		value->object = obj;
		obj->parent = value;
	}
	return value;
}

/* [한국어] json_create_value_array - 배열 타입의 JSON 값 생성.
 *          배열의 parent를 이 값으로 설정한다. */
static struct json_value *json_create_value_array(struct json_array *array)
{
	struct json_value *value = malloc(sizeof(struct json_value));

	if (value) {
		value->type = JSON_TYPE_ARRAY;
		value->array = array;
		array->parent = value;
	}
	return value;
}

/* 전방 선언: 상호 재귀적 해제 함수들 */
static void json_free_pair(struct json_pair *pair);
static void json_free_value(struct json_value *value);

/* [한국어] json_free_object - JSON 객체와 소속된 모든 쌍을 재귀적으로 해제 */
void json_free_object(struct json_object *obj)
{
	int i;

	for (i = 0; i < obj->pair_cnt; i++)
		json_free_pair(obj->pairs[i]);
	free(obj->pairs);
	free(obj);
}

/* [한국어] json_free_array - JSON 배열과 소속된 모든 값을 재귀적으로 해제 */
static void json_free_array(struct json_array *array)
{
	int i;

	for (i = 0; i < array->value_cnt; i++)
		json_free_value(array->values[i]);
	free(array->values);
	free(array);
}

/* [한국어] json_free_pair - JSON 쌍의 값과 이름을 해제 */
static void json_free_pair(struct json_pair *pair)
{
	json_free_value(pair->value);
	free(pair->name);
	free(pair);
}

/* [한국어] json_free_value - JSON 값을 타입에 따라 재귀적으로 해제.
 *          문자열이면 문자열 메모리, 객체/배열이면 하위 트리 전체를 해제한다. */
static void json_free_value(struct json_value *value)
{
	switch (value->type) {
	case JSON_TYPE_STRING:
		free(value->string);
		break;
	case JSON_TYPE_OBJECT:
		json_free_object(value->object);
		break;
	case JSON_TYPE_ARRAY:
		json_free_array(value->array);
		break;
	}
	free(value);
}

/* [한국어] json_array_add_value - 배열에 값을 추가.
 *          realloc으로 배열 포인터를 확장하고 부모 관계를 설정한다. */
static int json_array_add_value(struct json_array *array, struct json_value *value)
{
	struct json_value **values = realloc(array->values,
		sizeof(struct json_value *) * (array->value_cnt + 1));

	if (!values)
		return ENOMEM;
	values[array->value_cnt] = value;
	array->value_cnt++;
	array->values = values;

	value->parent_type = JSON_PARENT_TYPE_ARRAY;
	value->parent_array = array;
	return 0;
}

/* [한국어] json_object_add_pair - 객체에 키-값 쌍을 추가.
 *          realloc으로 쌍 배열을 확장하고 부모 관계를 설정한다. */
static int json_object_add_pair(struct json_object *obj, struct json_pair *pair)
{
	struct json_pair **pairs = realloc(obj->pairs,
		sizeof(struct json_pair *) * (obj->pair_cnt + 1));
	if (!pairs)
		return ENOMEM;
	pairs[obj->pair_cnt] = pair;
	obj->pair_cnt++;
	obj->pairs = pairs;

	pair->parent = obj;
	return 0;
}

/* [한국어] json_object_add_value_type - 객체에 임의 타입의 값을 추가하는 범용 함수.
 *          arg의 타입에 따라 적절한 json_value를 생성하고 pair로 묶어 추가한다.
 *          실패 시 생성된 중간 객체들을 정리한다. */
int json_object_add_value_type(struct json_object *obj, const char *name,
			       const struct json_value *arg)
{
	struct json_value *value;
	struct json_pair *pair;
	int ret;

	switch (arg->type) {
	case JSON_TYPE_STRING:
		value = json_create_value_string(arg->string);
		break;
	case JSON_TYPE_INTEGER:
		value = json_create_value_int(arg->integer_number);
		break;
	case JSON_TYPE_FLOAT:
		value = json_create_value_float(arg->float_number);
		break;
	case JSON_TYPE_OBJECT:
		value = json_create_value_object(arg->object);
		break;
	default:
	case JSON_TYPE_ARRAY:
		value = json_create_value_array(arg->array);
		break;
	}

	if (!value)
		return ENOMEM;

	pair = json_create_pair(name, value);
	if (!pair) {
		json_free_value(value);
		return ENOMEM;
	}
	ret = json_object_add_pair(obj, pair);
	if (ret) {
		json_free_pair(pair);
		return ENOMEM;
	}
	return 0;
}

/* [한국어] json_array_add_value_type - 배열에 임의 타입의 값을 추가하는 범용 함수.
 *          arg의 타입에 따라 적절한 json_value를 생성하고 배열에 추가한다. */
int json_array_add_value_type(struct json_array *array,
			      const struct json_value *arg)
{
	struct json_value *value;
	int ret;

	switch (arg->type) {
	case JSON_TYPE_STRING:
		value = json_create_value_string(arg->string);
		break;
	case JSON_TYPE_INTEGER:
		value = json_create_value_int(arg->integer_number);
		break;
	case JSON_TYPE_FLOAT:
		value = json_create_value_float(arg->float_number);
		break;
	case JSON_TYPE_OBJECT:
		value = json_create_value_object(arg->object);
		break;
	default:
	case JSON_TYPE_ARRAY:
		value = json_create_value_array(arg->array);
		break;
	}

	if (!value)
		return ENOMEM;

	ret = json_array_add_value(array, value);
	if (ret) {
		json_free_value(value);
		return ENOMEM;
	}
	return 0;
}

/*
 * [한국어] 아래 함수들은 JSON 트리의 들여쓰기 레벨을 계산한다.
 *          상호 재귀적으로 부모를 따라 올라가며 깊이를 구한다.
 *          루트 객체(parent==NULL)의 레벨은 0이다.
 */
static int json_value_level(struct json_value *value);
static int json_pair_level(struct json_pair *pair);
static int json_array_level(struct json_array *array);
static int json_object_level(struct json_object *object)
{
	if (object->parent == NULL)
		return 0;
	return json_value_level(object->parent);
}

static int json_pair_level(struct json_pair *pair)
{
	return json_object_level(pair->parent) + 1;
}

static int json_array_level(struct json_array *array)
{
	return json_value_level(array->parent);
}

static int json_value_level(struct json_value *value)
{
	if (value->parent_type == JSON_PARENT_TYPE_PAIR)
		return json_pair_level(value->parent_pair);
	else
		return json_array_level(value->parent_array) + 1;
}

/* [한국어] json_print_level - 들여쓰기 레벨만큼 공백(2칸씩)을 출력 */
static void json_print_level(int level, struct buf_output *out)
{
	while (level-- > 0)
		log_buf(out, "  ");
}

/* 전방 선언: 상호 재귀적 출력 함수들 */
static void json_print_pair(struct json_pair *pair, struct buf_output *);
static void json_print_value(struct json_value *value, struct buf_output *);

/* [한국어] json_print_object - JSON 객체를 "{...}" 형태로 출력.
 *          각 쌍 사이에 쉼표와 줄바꿈을 삽입한다. */
void json_print_object(struct json_object *obj, struct buf_output *out)
{
	int i;

	log_buf(out, "{\n");
	for (i = 0; i < obj->pair_cnt; i++) {
		if (i > 0)
			log_buf(out, ",\n");
		json_print_pair(obj->pairs[i], out);
	}
	log_buf(out, "\n");
	json_print_level(json_object_level(obj), out);
	log_buf(out, "}");
}

/* [한국어] json_print_pair - JSON 쌍을 "키" : 값 형태로 출력 */
static void json_print_pair(struct json_pair *pair, struct buf_output *out)
{
	json_print_level(json_pair_level(pair), out);
	log_buf(out, "\"%s\" : ", pair->name);
	json_print_value(pair->value, out);
}

/* [한국어] json_print_array - JSON 배열을 "[...]" 형태로 출력.
 *          각 값 사이에 쉼표와 줄바꿈을 삽입한다. */
static void json_print_array(struct json_array *array, struct buf_output *out)
{
	int i;

	log_buf(out, "[\n");
	for (i = 0; i < array->value_cnt; i++) {
		if (i > 0)
			log_buf(out, ",\n");
		json_print_level(json_value_level(array->values[i]), out);
		json_print_value(array->values[i], out);
	}
	log_buf(out, "\n");
	json_print_level(json_array_level(array), out);
	log_buf(out, "]");
}

/* [한국어] json_print_value - JSON 값을 타입에 따라 출력.
 *          문자열은 큰따옴표로 감싸고, 객체/배열은 재귀적으로 출력한다. */
static void json_print_value(struct json_value *value, struct buf_output *out)
{
	switch (value->type) {
	case JSON_TYPE_STRING:
		log_buf(out, "\"%s\"", value->string);
		break;
	case JSON_TYPE_INTEGER:
		log_buf(out, "%lld", value->integer_number);
		break;
	case JSON_TYPE_FLOAT:
		log_buf(out, "%f", value->float_number);
		break;
	case JSON_TYPE_OBJECT:
		json_print_object(value->object, out);
		break;
	case JSON_TYPE_ARRAY:
		json_print_array(value->array, out);
		break;
	}
}
