/*
 * [한국어 설명] 메모리 잠금 및 스레드 메모리 접근 테스트 (memlock.c)
 *
 * === 파일의 역할 ===
 * 다수의 스레드를 생성하여 각 스레드가 지정된 크기의 메모리를 할당하고 반복적으로 접근하는
 * 테스트 프로그램이다. mlockall() 등의 메모리 고정 동작을 검증하기 위해 사용되며,
 * 멀티스레드 환경에서 대량 메모리 접근 시의 동작을 확인하는 데 목적이 있다.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static struct thread_data {
	unsigned long mib;
} td;

static void *worker(void *data)
{
	struct thread_data *td = data;
	unsigned long index;
	size_t size;
	char *buf;
	int i, first = 1;

	size = td->mib * 1024UL * 1024UL;
	buf = malloc(size);

	for (i = 0; i < 100000; i++) {
		for (index = 0; index + 4096 < size; index += 4096)
			memset(&buf[index+512], 0x89, 512);
		if (first) {
			printf("loop%d: did %lu MiB\n", i+1, td->mib);
			first = 0;
		}
	}
	free(buf);
	return NULL;
}

int main(int argc, char *argv[])
{
	unsigned long mib, threads;
	pthread_t *pthreads;
	int i;

	if (argc < 3) {
		printf("%s: <MiB per thread> <threads>\n", argv[0]);
		return 1;
	}

	mib = strtoul(argv[1], NULL, 10);
	threads = strtoul(argv[2], NULL, 10);
	if (threads < 1 || threads > 65536) {
		printf("%s: invalid 'threads' argument\n", argv[0]);
		return 1;
	}

	pthreads = calloc(threads, sizeof(pthread_t));
	td.mib = mib;

	for (i = 0; i < threads; i++)
		pthread_create(&pthreads[i], NULL, worker, &td);

	for (i = 0; i < threads; i++) {
		void *ret;

		pthread_join(pthreads[i], &ret);
	}
	return 0;
}
