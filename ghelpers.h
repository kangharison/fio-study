/*
 * [한국어 설명] gfio GTK 위젯 도우미 헤더 (ghelpers.h)
 *
 * === 파일의 역할 ===
 * gfio���서 반복적으로 사용되는 GTK 위젯 생성 패턴을 유틸리티 함수로 제공한다.
 * 프레임 안에 엔트리/라벨/콤보박스를 생성하거나, 트리뷰 컬럼을 설정하는 등의
 * 공통 UI 구성 작업을 간소화한다.
 *
 * === multitext_widget ===
 * 하나의 GtkEntry에 여러 ���스트를 번갈아 표시할 수 있는 위젯.
 * 여러 Job의 I/O 타입, 블록 크기 등을 하나의 엔트리에서 전환하며 표시할 때 사용.
 */
#ifndef GFIO_HELPERS_H
#define GFIO_HELPERS_H

/* 프레임 안에 콤보박스 생성 (드롭다운 선택 위젯) */
GtkWidget *new_combo_entry_in_frame(GtkWidget *box, const char *label);
/* 프레임 안에 읽기 전용 엔트리 생성 (정보 표시용) */
GtkWidget *new_info_entry_in_frame(GtkWidget *box, const char *label);
/* 프레임 안에 라벨 위젯 생성 */
GtkWidget *new_info_label_in_frame(GtkWidget *box, const char *label);
/* 프레임 안에 색상 지정된 읽기 전용 엔트리 생성 (RGB 텍스트 색상) */
GtkWidget *new_info_entry_in_frame_rgb(GtkWidget *box, const char *label,
					gfloat r, gfloat g, gfloat b);
/* 스핀 버튼 생성 (숫자 입력용, 최소/최대/기본값 지정) */
GtkWidget *create_spinbutton(GtkWidget *hbox, double min, double max, double defval);
/* 라벨에 정수 값 설정 */
void label_set_int_value(GtkWidget *entry, unsigned int val);
/* 엔트리에 정수 값 설정 */
void entry_set_int_value(GtkWidget *entry, unsigned int val);

/* 스크롤 가능한 윈도우 생성 (자동 스크롤바 정책) */
GtkWidget *get_scrolled_window(gint border_width);

/*
 * 멀티텍스트 위젯 - 하나의 GtkEntry에 여러 텍스트를 전환하며 표시
 * 예: 여러 Job의 ioengine 이름을 하나의 필드에서 번갈아 표시
 */
struct multitext_widget {
	GtkWidget *entry;        /* 텍스트를 표시할 GtkEntry 위젯 */
	char **text;             /* 텍스트 문자열 배열 */
	unsigned int cur_text;   /* 현재 표시 중인 텍스트 인덱스 */
	unsigned int max_text;   /* 텍스트 배열의 총 개수 */
};

void multitext_add_entry(struct multitext_widget *mt, const char *text);
void multitext_set_entry(struct multitext_widget *mt, unsigned int index);
void multitext_update_entry(struct multitext_widget *mt, unsigned int index,
			    const char *text);
void multitext_free(struct multitext_widget *mt);

/* 트리뷰 컬럼 정렬 및 표시 플래그 */
#define ALIGN_LEFT 1      /* 좌측 정렬 */
#define ALIGN_RIGHT 2     /* 우측 정렬 */
#define INVISIBLE 4       /* 컬럼 숨김 */
#define UNSORTABLE 8      /* 정렬 비활성화 */

/* 트리뷰에 컬럼 추가 (제목, 정렬, 크기 조절 등 설정) */
GtkTreeViewColumn *tree_view_column(GtkWidget *tree_view, int index, const char *title, unsigned int flags);

#endif
