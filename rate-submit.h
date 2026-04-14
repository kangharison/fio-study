/*
 * [한국어] rate-submit.h - 속도 제한 I/O 제출 헤더
 *
 * io_submit_mode=offload일 때 워크큐 기반 I/O 제출을 위한 인터페이스를 정의한다.
 * rate_submit_init()으로 워크큐를 초기화하고, rate_submit_exit()으로 종료한다.
 
 * === 파일의 역할 ===
 * io_submit_mode=offload 워크큐 기반 I/O 제출 인터페이스를 선언한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * rate-submit.c와 짝을 이루는 헤더. backend.c에서 참조.
 *
 * === 타 모듈과의 연결 ===
 * - rate-submit.c: 이 헤더의 함수 구현
 * - backend.c: rate_submit_init/exit 호출
 *
 * === 주요 함수/구조체 요약 ===
 * - rate_submit_init(): 워크큐 초기화
 * - rate_submit_exit(): 워크큐 종료
 */
#ifndef FIO_RATE_SUBMIT
#define FIO_RATE_SUBMIT

/* 속도 제한 워크큐 초기화 (offload 모드일 때만 실제 동작) */
int rate_submit_init(struct thread_data *, struct sk_out *);

/* 속도 제한 워크큐 종료 및 워커 스레드 정리 */
void rate_submit_exit(struct thread_data *);

#endif
