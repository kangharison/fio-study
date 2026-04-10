/*
 * [한국어 설명] gfio 옵션 편집 창 헤더 (goptions.h)
 *
 * fio job 옵션을 GTK 다이얼로그로 편집할 수 있게 하는 모듈.
 * - gopt_get_options_window(): 옵션 편집 창을 열어 job 파라미터를 GUI로 조정
 * - gopt_init()/gopt_exit(): 옵션 그룹 시스템 초기화/정리
 */
#ifndef GFIO_OPTIONS_H
#define GFIO_OPTIONS_H

#include <gtk/gtk.h>

/* 옵션 편집 창 열기 - fio job 옵션을 GTK 위젯으로 표시/수정 */
void gopt_get_options_window(GtkWidget *window, struct gfio_client *gc);
/* 옵션 시스템 초기화 (fio 옵션 그룹 파싱) */
void gopt_init(void);
/* 옵션 시스템 정리 */
void gopt_exit(void);

#endif
