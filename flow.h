/*
 * [한국어] flow.h - I/O 흐름(flow) 제어 헤더
 *
 * 여러 fio 작업 간의 I/O 속도 비율을 조절하는 흐름 제어 인터페이스를 정의한다.
 * 같은 flow_id 그룹 내에서 각 작업의 flow weight에 비례하여 I/O가 분배된다.
 */
#ifndef FIO_FLOW_H
#define FIO_FLOW_H

/* [한국어] 흐름 가중치 최대값 - 단일 작업에 설정 가능한 최대 weight */
#define FLOW_MAX_WEIGHT 1000

/* [한국어] 현재 스레드의 I/O 비율이 임계치를 초과했는지 검사 (1=초과, 0=정상) */
int flow_threshold_exceeded(struct thread_data *td);
/* [한국어] 작업 시작 시 흐름 초기화 - 흐름 객체 획득 및 weight 등록 */
void flow_init_job(struct thread_data *td);
/* [한국어] 작업 종료 시 흐름 정리 - 흐름 참조 해제 */
void flow_exit_job(struct thread_data *td);

/* [한국어] 전역 흐름 서브시스템 종료 정리 */
void flow_exit(void);
/* [한국어] 전역 흐름 서브시스템 초기화 */
void flow_init(void);

#endif
