/*
 * [한국어 설명] 그래프 렌더링 라이브러리 헤더 (graph.h)
 *
 * === 파일의 역할 ===
 * Cairo 2D 그래픽 라이브러리를 사용하여 라인 그래프와 막대 그래프를
 * 렌더링하는 기능을 제공한다. gfio에서 IOPS, 대역폭, 레이턴시 분포를
 * 시각적으로 표시하는 데 사용된다.
 *
 * === 사용 흐름 ===
 * 1. graph_new()로 그래프 객체 생성 (크기, 폰트 지정)
 * 2. graph_title(), graph_x_title(), graph_y_title()로 제목 설정
 * 3. graph_add_label()로 데이터 계열(라벨) 추가 (예: "Read IOPS", "Write IOPS")
 * 4. graph_set_color()로 각 계열의 색상 설정
 * 5. graph_add_data() 또는 graph_add_xy_data()로 데이터 포인트 추가
 * 6. line_graph_draw() 또는 bar_graph_draw()로 Cairo 컨텍스트에 렌더링
 * 7. graph_free()로 해제
 */
#ifndef GRAPH_H
#define GRAPH_H

struct graph;        /* 그래프 객체 (내부 구현은 graph.c에 정의) */
struct graph_label;  /* 데이터 계열(라벨) 객체 */

typedef struct graph_label * graph_label_t;  /* 라벨 핸들 타입 */

#define GRAPH_DEFAULT_FONT	"Sans 12"   /* 기본 그래프 폰트 */

struct graph *graph_new(unsigned int xdim, unsigned int ydim, const char *font);
/* graph_new() 지정된 크기와 폰트로 새 그래프 구조체를 생성하여 반환 */
void graph_set_size(struct graph *g, unsigned int xdim, unsigned int ydim);
/* graph_set_size() Changes the size of a graph to the given dimensions. */ 
void graph_set_position(struct graph *g, double xoffset, double yoffset);
/* graph_set_position() sets the x- and y-offset to translate the graph */
void bar_graph_draw(struct graph *g, cairo_t *cr);
/* bar_graph_draw() draws the given graph as a bar graph */
void line_graph_draw(struct graph *g, cairo_t *cr);
/* line_graph_draw draws the given graph as a line graph */
void line_graph_set_data_count_limit(struct graph *g, int per_label_limit);
/* line_graph_set_data_count_limit() limits the amount of data which can
 * be added to a line graph.  Once the limit is reached, the oldest data 
 * is discarded as new data is added
 */
void graph_set_font(struct graph *g, const char *font);
void graph_title(struct graph *g, const char *title);
/* graph_title() sets the main title of the graph to the given string */
void graph_x_title(struct graph *g, const char *title);
/* graph_x_title() sets the title of the x axis to the given string */
void graph_y_title(struct graph *g, const char *title);
/* graph_y_title() sets the title of the y axis to the given string */
graph_label_t graph_add_label(struct graph *g, const char *label);
/* graph_add_label() adds a new "stream" of data to be graphed.
 * For line charts, each label is a separate line on the graph.
 * For bar charts, each label is a grouping of columns on the x-axis
 * For example:
 *
 *  |  *                          | **
 *  |   *      xxxxxxxx           | **
 *  |    ***  x                   | **              **
 *  |       *x       ****         | **      **      **
 *  |    xxxx*  *****             | ** xx   ** xx   **
 *  |   x     **                  | ** xx   ** xx   ** xx
 *  |  x                          | ** xx   ** xx   ** xx
 *  -----------------------       -------------------------
 *                                    A       B       C
 *
 * For a line graph, the 'x's     For a bar graph, 
 * would be on one "label", and   'A', 'B', and 'C'
 * the '*'s would be on another   are the labels.
 * label.
 */

int graph_add_data(struct graph *g, graph_label_t label, const double value);
/* graph_add_data() is used to add data to the labels of a bar graph */
int graph_add_xy_data(struct graph *g, graph_label_t label,
		const double x, const double y, const char *tooltip);
/* graph_add_xy_data is used to add data to the labels of a line graph */

void graph_set_color(struct graph *g, graph_label_t label,
		double red, double green, double blue);
#define INVISIBLE_COLOR (-1.0)
/* graph_set_color is used to set the color used to plot the data in
 * a line graph.  INVISIBLE_COLOR can be used to plot the data invisibly.
 * Invisible data will have the same effect on the scaling of the axes
 * as visible data.
 */

void graph_free(struct graph *bg);
/* free a graph allocated by graph_new() */

typedef void (*graph_axis_unit_change_callback)(struct graph *g, int power_of_ten);
void graph_x_axis_unit_change_notify(struct graph *g, graph_axis_unit_change_callback f);
void graph_y_axis_unit_change_notify(struct graph *g, graph_axis_unit_change_callback f);
/* The labels used on the x and y axes may be shortened.  You can register for callbacks
 * so that you can know how the labels are shorted, typically used to adjust the axis
 * titles to display the proper units.  The power_of_ten parameter indicates what power
 * of ten the labels have been divided by (9, 6, 3, or 0, corresponding to billions,
 * millions, thousands and ones. 
 */ 

void graph_add_extra_space(struct graph *g, double left_percent, double right_percent,
				double top_percent, double bottom_percent);
/* graph_add_extra_space() adds extra space to edges of the the graph
 * so that the data doesn't go to the very edges.
 */

extern int graph_has_tooltips(struct graph *g);
extern const char *graph_find_tooltip(struct graph *g, int x, int y);
extern int graph_contains_xy(struct graph *p, int x, int y);

extern void graph_set_base_offset(struct graph *g, unsigned int base_offset);
extern void graph_set_graph_all_zeroes(struct graph *g, unsigned int set);

extern void graph_clear_values(struct graph *g);

#endif

