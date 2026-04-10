/*
 * [한국어 설명] Cairo 텍스트 도우미 헤더 (cairo_text_helpers.h)
 *
 * Cairo 2D 그래픽 라이브러리에서 텍스트를 정렬하여 그리는 유틸리티 함수 선언.
 * 그래프의 축 라벨, 제목, 눈금 값 등을 정확한 위치에 렌더링할 때 사용된다.
 */
#ifndef CAIRO_TEXT_HELPERS_H
#define CAIRO_TEXT_HELPERS_H

#include <cairo.h>

/* 지정 좌표를 중심으로 텍스트를 수평 중앙 정렬하여 그리기 */
void draw_centered_text(cairo_t *cr, const char *font, double x, double y,
			       double fontsize, const char *text);

/* 지정 좌표를 기준으로 텍스트를 우측 정렬하여 그리기 */
void draw_right_justified_text(cairo_t *cr, const char *font,
				double x, double y,
				double fontsize, const char *text);

/* 지정 좌표를 기준으로 텍스트를 좌측 정렬하여 그리기 */
void draw_left_justified_text(cairo_t *cr, const char *font,
				double x, double y,
				double fontsize, const char *text);

/* 텍스트를 90도 회전하여 세로로 중앙 정렬하여 그리기 (Y축 제목 등에 사용) */
void draw_vertical_centered_text(cairo_t *cr, const char *font, double x,
					double y, double fontsize,
					const char *text);
#endif
