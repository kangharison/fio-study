/*
 * [한국어 설명] 단일 파일 퍼즈 테스트 하네스 (onefile.c)
 *
 * === 파일의 역할 ===
 * 퍼즈 테스트를 libFuzzer 없이 독립적으로 실행할 수 있게 해주는 하네스 프로그램이다.
 * 명령줄로 지정된 파일을 읽어 메모리에 로드한 뒤 LLVMFuzzerTestOneInput 함수에
 * 전달하여 단일 입력에 대한 퍼즈 대상 함수의 동작을 검증한다.
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size);

int main(int argc, char** argv)
{
	FILE *fp;
	uint8_t *data;
	size_t size;

	if (argc != 2)
		return 1;

	/* opens the file, get its size, and reads it into a buffer */
	fp = fopen(argv[1], "rb");
	if (fp == NULL)
		return 2;

	if (fseek(fp, 0L, SEEK_END) != 0) {
		fclose(fp);
		return 2;
	}
	size = ftell(fp);
	if (size == (size_t) -1) {
		fclose(fp);
		return 2;
	}
	if (fseek(fp, 0L, SEEK_SET) != 0) {
		fclose(fp);
		return 2;
	}
	data = malloc(size);
	if (data == NULL) {
		fclose(fp);
		return 2;
	}
	if (fread(data, size, 1, fp) != 1) {
		fclose(fp);
		free(data);
		return 2;
	}

	/* launch fuzzer */
	LLVMFuzzerTestOneInput(data, size);
	free(data);
	fclose(fp);

	return 0;
}
