/*
 * [한국어 설명] INI/작업 파일 파서 퍼즈 테스트 (fuzz_parseini.c)
 *
 * === 파일의 역할 ===
 * AFL이나 libFuzzer 같은 퍼징 도구와 함께 사용되는 퍼즈 테스트 대상 함수를
 * 구현한다. LLVMFuzzerTestOneInput 인터페이스를 통해 무작위 데이터를 fio의
 * parse_jobs_ini 함수에 전달하여 파서의 견고성과 크래시 여부를 검증한다.
 */
#include "fio.h"

static int initialized = 0;

const char *const fakeargv[] = {(char *) "fuzz",
	(char *) "--output", (char *) "/dev/null",
	(char *) "--parse-only",
	0};

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
	char *fuzzedini;

	if (size < 2)
		return 0;

	if (initialized == 0) {
		if (fio_init_options()) {
			printf("Failed fio_init_options\n");
			return 1;
		}

		parse_cmd_line(4, (char **) fakeargv, 0);
		sinit();

		initialized = 1;
	}
	fuzzedini = malloc(size);
	if (!fuzzedini) {
		printf("Failed malloc\n");
		return 1;
	}
	/* final character is type for parse_jobs_ini */
	memcpy(fuzzedini, data, size - 1);
	/* ensures final 0 */
	fuzzedini[size - 1] = 0;

	parse_jobs_ini(fuzzedini, 1, 0, data[size - 1]);
	free(fuzzedini);
	return 0;
}
