/*
 * [한국어] rate-submit.h - 속도 제한 I/O 제출 헤더
 *
 * io_submit_mode=offload일 때 워크큐 기반 I/O 제출을 위한 인터페이스를 정의한다.
 * rate_submit_init()으로 워크큐를 초기화하고, rate_submit_exit()으로 종료한다.
 */
#ifndef FIO_RATE_SUBMIT
#define FIO_RATE_SUBMIT

/* 속도 제한 워크큐 초기화 (offload 모드일 때만 실제 동작) */
int rate_submit_init(struct thread_data *, struct sk_out *);

/* 속도 제한 워크큐 종료 및 워커 스레드 정리 */
void rate_submit_exit(struct thread_data *);

#endif
