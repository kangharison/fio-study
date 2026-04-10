/*
 * [한국어 설명] gfio 클라이언트 헤더 파일 (gclient.h)
 *
 * gfio에서 fio 서버와의 통신을 처리하는 클라이언트 모듈의 인터페이스를 정의한다.
 * gfio_client_ops는 서버로부터의 이벤트(텍스트 메시지, 통계, ETA 등)를 처리하는
 * 콜백 함수 집합이며, fio 클라이언트 프레임워크에 등록된다.
 *
 * RGB 색상 상수는 그래프에서 Read(초록)/Write(빨강)/Trim(보라) 데이터를
 * 구분하여 표시하는 데 사용된다.
 */
#ifndef GFIO_CLIENT_H
#define GFIO_CLIENT_H

/* gfio 전용 클라이언트 이벤트 핸들러 (콜백 함수 집합) */
extern struct client_ops gfio_client_ops;

/* 완료된 Job의 최종 결과를 결과 창에 표시 */
extern void gfio_display_end_results(struct gfio_client *);

/* 그래프 색상 정의 (RGB, 0.0~1.0) */
#define GFIO_READ_R	0.13    /* 읽기: 초록색 (R) */
#define GFIO_READ_G	0.54    /* 읽기: 초록색 (G) */
#define GFIO_READ_B	0.13    /* 읽기: 초록색 (B) */
#define GFIO_WRITE_R	1.00    /* 쓰기: 빨간색 (R) */
#define GFIO_WRITE_G	0.00    /* 쓰기: 빨간색 (G) */
#define GFIO_WRITE_B	0.00    /* 쓰기: 빨간색 (B) */
#define GFIO_TRIM_R	0.24    /* 트림: 보라색 (R) */
#define GFIO_TRIM_G	0.18    /* 트림: 보라색 (G) */
#define GFIO_TRIM_B	0.52    /* 트림: 보라색 (B) */

#endif
