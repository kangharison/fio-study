/*
 * [한국어 설명] GTK+ 버전 호환성 래퍼 헤더 (gcompat.h)
 *
 * === 파일의 역할 ===
 * GTK+ 2.x의 다양한 마이너 버전 간 API 차이를 추상화하는 호환성 래퍼이다.
 * GTK 2.24 미만에서 누락된 GtkComboBoxText API, GTK 2.14 미만의
 * gtk_dialog_get_content_area() 등을 폴리필(polyfill)로 제공한다.
 * GTK 2와 GTK 3 간의 그리기 이벤트 이름 차이("expose_event" vs "draw")도 처리한다.
 */
#ifndef GFIO_GTK_COMPAT
#define GFIO_GTK_COMPAT

#include <gtk/gtk.h>

/* GTK 2.24 미만: GtkComboBoxText API가 없으므로 GtkComboBox로 폴리필 */
#if GTK_MAJOR_VERSION <= 2 && GTK_MINOR_VERSION < 24
struct GtkComboBoxText;
typedef GtkComboBox GtkComboBoxText;
GtkWidget *gtk_combo_box_text_new(void);
GtkWidget *gtk_combo_box_text_new_with_entry(void);
void gtk_combo_box_text_append_text(GtkComboBoxText *combo_box, const gchar *text);
void gtk_combo_box_text_insert_text(GtkComboBoxText *combo_box, gint position, const gchar *text);
void gtk_combo_box_text_prepend_text(GtkComboBoxText *combo_box, const gchar *text);
void gtk_combo_box_text_remove(GtkComboBoxText *combo_box, gint position);
gchar *gtk_combo_box_text_get_active_text(GtkComboBoxText *combo_box);

#define GTK_COMBO_BOX_TEXT	GTK_COMBO_BOX
#endif /* GTK_MAJOR_VERSION <= 2 && GTK_MINOR_VERSION < 24 */

#if GTK_MAJOR_VERSION <= 2 && GTK_MINOR_VERSION < 14
static inline GtkWidget *gtk_dialog_get_content_area(GtkDialog *dialog)
{
	return dialog->vbox;
}
static inline GdkWindow *gtk_widget_get_window(GtkWidget *w)
{
	return w->window;
}
#endif

#if GTK_MAJOR_VERSION < 3
guint gtk_widget_get_allocated_width(GtkWidget *w);
guint gtk_widget_get_allocated_height(GtkWidget *w);
#endif

#if GTK_MAJOR_VERSION == 3
#define GFIO_DRAW_EVENT		"draw"
#elif GTK_MAJOR_VERSION == 2
#define GFIO_DRAW_EVENT		"expose_event"
#endif

#if GTK_MAJOR_VERSION <= 2 && GTK_MINOR_VERSION < 18
void gtk_widget_set_can_focus(GtkWidget *widget, gboolean can_focus);
#endif

#endif
