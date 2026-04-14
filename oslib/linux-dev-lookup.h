/*
 * [한국어 설명] Linux 블록 장치 검색 헤더 (linux-dev-lookup.h)
 *
 * === 파일의 역할 ===
 * blktrace 재생(replay) 시 major:minor 번호로 블록 장치 노드를 찾는 함수의 선언부이다.
 * blktrace 로그에는 장치가 major:minor 번호로 기록되므로,
 * 이를 실제 장치 경로(/dev/sdX)로 변환하는 기능이 필요하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * fio의 blktrace 재생 기능에서 사용된다:
 *   iolog.c (blktrace 재생) → blktrace_lookup_device()
 *
 * === 타 모듈과의 연결 ===
 * - oslib/linux-dev-lookup.c: 실제 구현부
 * - iolog.c: blktrace 재생 시 장치 경로 해석에 사용
 *
 * === 주요 함수 요약 ===
 * - blktrace_lookup_device(): major:minor로 블록 장치 노드를 재귀 검색
 */
#ifndef LINUX_DEV_LOOKUP
#define LINUX_DEV_LOOKUP

int blktrace_lookup_device(const char *redirect, char *path, unsigned int maj,
			   unsigned int min);

#endif
