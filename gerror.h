/*
 * [한국어 설명] gfio 에러 보고 헤더 (gerror.h)
 *
 * GUI에서 사용자에게 에러 및 정보 메시지를 표시하기 위한 함수 선언.
 * - gfio_report_error: 인포바(InfoBar) 형태로 에러 메시지를 표시
 * - gfio_report_info: 모달 다이얼로그로 정보 메시지를 표시
 */
#ifndef GFIO_ERROR_H
#define GFIO_ERROR_H

/* gui_entry에 에러 메시지를 인포바로 표시 (printf 스타일 포맷) */
extern void gfio_report_error(struct gui_entry *ge, const char *format, ...);
/* 메인 윈도우에 정보 다이얼로그를 표시 */
extern void gfio_report_info(struct gui *ui, const char *title, const char *message);

#endif
