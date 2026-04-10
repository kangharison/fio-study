/*
 * [한국어 설명] 아키텍처 감지 스텁 (arch.c)
 *
 * === 파일의 역할 ===
 * 테스트 프로그램을 독립적으로 빌드할 때 필요한 아키텍처 관련 전역 변수를
 * 정의하는 스텁 파일이다. arch_flags와 arch_random 변수를 기본값으로
 * 초기화하여 fio 본체의 아키텍처 감지 모듈을 대체한다.
 */
#include "../arch/arch.h"

unsigned long arch_flags = 0;
int arch_random;
