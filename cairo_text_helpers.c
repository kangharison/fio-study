/*
 * [한국어 설명] Cairo 텍스트 도우미 구현 (cairo_text_helpers.c)
 *
 * === 파일의 역할 ===
 * Cairo 그래픽 라이브러리에서 텍스트를 다양한 정렬(좌측/중앙/우측/세로)로
 * 렌더링하는 유틸리티 함수를 구현한다.
 *
 * === 정렬 방식 ===
 * Cairo의 text_extents를 이용하여 텍스트의 실제 렌더링 크기를 측정한 후,
 * 지정된 좌표를 기준으로 적절히 오프셋을 계산하여 정확한 위치에 배치한다.
 * 세로 텍스트는 Cairo의 translate + rotate 변환을 사용하여 -90도 회전시킨다.
 */
#include "cairo_text_helpers.h"

#include <cairo.h>
#include <gtk/gtk.h>
#include <math.h>

/* 내부 구현: 지정된 정렬 방식으로 텍스트를 렌더링하는 공통 함수 */
static void draw_aligned_text(cairo_t *cr, const char *font, double x, double y,
			       double fontsize, const char *text, int alignment)
{
#define CENTERED 0
#define LEFT_JUSTIFIED 1
#define RIGHT_JUSTIFIED 2

	double factor, direction;
	cairo_text_extents_t extents;

	switch (alignment) {
		case CENTERED:
			direction = -1.0;
			factor = 0.5;
			break;
		case RIGHT_JUSTIFIED:
			direction = -1.0;
			factor = 1.0;
			break;
		case LEFT_JUSTIFIED:
		default:
			direction = 1.0;
			factor = 0.0;
			break;
	}
	cairo_select_font_face(cr, font, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

	cairo_set_font_size(cr, fontsize);
	cairo_text_extents(cr, text, &extents);
	x = x + direction * (factor * extents.width  + extents.x_bearing);
	y = y - (extents.height / 2 + extents.y_bearing);

	cairo_move_to(cr, x, y);
	cairo_show_text(cr, text);
}

void draw_centered_text(cairo_t *cr, const char *font, double x, double y,
			       double fontsize, const char *text)
{
	draw_aligned_text(cr, font, x, y, fontsize, text, CENTERED);
}

void draw_right_justified_text(cairo_t *cr, const char *font,
				double x, double y,
				double fontsize, const char *text)
{
	draw_aligned_text(cr, font, x, y, fontsize, text, RIGHT_JUSTIFIED);
}

void draw_left_justified_text(cairo_t *cr, const char *font,
				double x, double y,
				double fontsize, const char *text)
{
	draw_aligned_text(cr, font, x, y, fontsize, text, LEFT_JUSTIFIED);
}

void draw_vertical_centered_text(cairo_t *cr, const char *font, double x,
					double y, double fontsize,
					const char *text)
{
	double sx, sy;
	cairo_text_extents_t extents;

	cairo_select_font_face(cr, font, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

	cairo_set_font_size(cr, fontsize);
	cairo_text_extents(cr, text, &extents);
	sx = x;
	sy = y;
	y = y + (extents.width / 2.0 + extents.x_bearing);
	x = x - (extents.height / 2.0 + extents.y_bearing);

	cairo_move_to(cr, x, y);
	cairo_save(cr);
	cairo_translate(cr, -sx, -sy);
	cairo_rotate(cr, -90.0 * M_PI / 180.0);
	cairo_translate(cr, sx, sy);
	cairo_show_text(cr, text);
	cairo_restore(cr);
}

