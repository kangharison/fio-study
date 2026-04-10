/*
 * [한국어 설명] gfio 인쇄 기능 헤더 (printing.h)
 *
 * GTK의 인쇄 프레임워크(GtkPrintOperation)를 사용하여
 * 벤치마크 결과를 프린터로 출력하는 기능의 인터페이스.
 */
#ifndef PRINTING_H
#define PRINTING_H

/* 현재 gui_entry의 벤치마크 결과를 프린터로 인쇄 (인�� 다이얼로그 표시) */
void gfio_print_results(struct gui_entry *ge);

#endif
