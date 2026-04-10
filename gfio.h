/*
 * [한국어 설명] gfio 메인 GUI 헤더 파일 (gfio.h)
 *
 * === 파일의 역할 ===
 * gfio(GUI Front-end for fio)의 핵심 데이터 구조체를 정의한다.
 * GTK+ 기반의 그래픽 사용자 인터페이스를 통해 fio의 I/O 벤치마크를
 * 시각적으로 구성, 실행, 모니터링할 수 있게 한다.
 *
 * === 주요 구조체 계층 ===
 * - gui: 메인 윈도우 (애플리케이션 전체를 관리)
 *   └─ gui_entry: 탭(노트북 페이지)별 GUI 엔트리 (서버당 하나)
 *       ├─ gfio_client: fio 서버 연결 및 결과 데이터
 *       ├─ gfio_graphs: IOPS/대역폭 실시간 그래프
 *       ├─ probe_widget: 서버 정보 표시 (호스트명, OS, 아키텍처)
 *       └─ eta_widget: ETA(예상 완료 시간) 정보 표시
 *
 * === 상태 머신 (GE_STATE_*) ===
 * NEW → CONNECTED → JOB_SENT → JOB_STARTED → JOB_RUNNING → JOB_DONE
 * 각 상태에 따라 Connect/Send/Start 버튼의 활성화 상태가 변경된다.
 */

#ifndef GFIO_H
#define GFIO_H

#include <gtk/gtk.h>

#include "gcompat.h"         /* GTK 버전별 호환성 래퍼 */
#include "stat.h"            /* thread_stat, group_run_stats 등 통계 구조체 */
#include "thread_options.h"  /* thread_options 구조체 (job 설정) */
#include "ghelpers.h"        /* GTK 위젯 도우미 함수 */
#include "graph.h"           /* 그래프 렌더링 라이브러리 */

/* 서버 프로브(probe) 정보 표시 위젯 - 연결된 fio 서버의 기본 정보를 표시 */
struct probe_widget {
	GtkWidget *hostname;
	GtkWidget *os;
	GtkWidget *arch;
	GtkWidget *fio_ver;
};

/* ETA(예상 완료 시간) 정보 표시 위젯 - I/O 테스트 진행 상황을 실시간으로 표시 */
struct eta_widget {
	GtkWidget *names;
	struct multitext_widget iotype;
	struct multitext_widget bs;
	struct multitext_widget ioengine;
	struct multitext_widget iodepth;
	GtkWidget *jobs;
	GtkWidget *files;
	GtkWidget *read_bw;
	GtkWidget *read_iops;
	GtkWidget *cr_bw;
	GtkWidget *cr_iops;
	GtkWidget *write_bw;
	GtkWidget *write_iops;
	GtkWidget *cw_bw;
	GtkWidget *cw_iops;
	GtkWidget *trim_bw;
	GtkWidget *trim_iops;
};

/* 그래프 위젯 - IOPS 및 대역폭(Bandwidth) 실시간 라인 그래프를 관리 */
struct gfio_graphs {
#define DRAWING_AREA_XDIM 1000   /* 그래프 영역 가로 크기 (픽셀) */
#define DRAWING_AREA_YDIM 400    /* 그래프 영역 세로 크기 (픽셀) */
	GtkWidget *drawing_area;
	struct graph *iops_graph;
	graph_label_t read_iops;
	graph_label_t write_iops;
	graph_label_t trim_iops;
	struct graph *bandwidth_graph;
	graph_label_t read_bw;
	graph_label_t write_bw;
	graph_label_t trim_bw;
};

/*
 * Main window widgets and data
 * [한국어] 메인 윈도우 위젯 및 데이터 - gfio 애플리케이션의 최상위 GUI 구조체
 * 메뉴, 노트북(탭), 로그 뷰, 그래프, ETA 표시 등 모든 메인 UI 요소를 포함한다.
 */
struct gui {
	GtkUIManager *uimanager;
	GtkRecentManager *recentmanager;
	GtkActionGroup *actiongroup;
	guint recent_ui_id;
	GtkWidget *menu;
	GtkWidget *window;
	GtkWidget *vbox;
	GtkWidget *thread_status_pb;
	GtkWidget *buttonbox;
	GtkWidget *notebook;
	GtkWidget *error_info_bar;
	GtkWidget *error_label;
	GtkListStore *log_model;
	GtkWidget *log_tree;
	GtkWidget *log_view;
	struct gfio_graphs graphs;
	struct probe_widget probe;
	struct eta_widget eta;
	pthread_t server_t;

	pthread_t t;
	int handler_running;

	GHashTable *ge_hash;
};

extern struct gui main_ui;

/* gui_entry 상태 머신 - 서버 연결부터 job 완료까지의 상태 전이를 정의 */
enum {
	GE_STATE_NEW = 1,        /* 초기 상태: 미연결 */
	GE_STATE_CONNECTED,      /* 서버 연결됨 */
	GE_STATE_JOB_SENT,       /* Job 설정이 서버에 전송됨 */
	GE_STATE_JOB_STARTED,    /* Job 시작됨 */
	GE_STATE_JOB_RUNNING,    /* Job 실행 중 (I/O 진행) */
	GE_STATE_JOB_DONE,       /* Job 완료 */
};

/* 버튼 인덱스 - gui_entry의 button[] 배열에서 각 버튼을 식별 */
enum {
	GFIO_BUTTON_CONNECT = 0, /* 서버 연결/해제 버튼 */
	GFIO_BUTTON_SEND,        /* Job 설정 전송 버튼 */
	GFIO_BUTTON_START,       /* Job 시작 버튼 */
	GFIO_BUTTON_NR,          /* 버튼 총 개수 */
};

/*
 * Notebook entry
 * [한국어] 노트북(탭) 엔트리 - 각 fio 서버 연결을 하나의 탭으로 관리
 * 서버별로 독립적인 버튼, 그래프, 로그, 결과 창을 가진다.
 */
struct gui_entry {
	struct gui *ui;

	GtkWidget *vbox;
	GtkWidget *job_notebook;
	GtkWidget *thread_status_pb;
	GtkWidget *buttonbox;
	GtkWidget *button[GFIO_BUTTON_NR];
	GtkWidget *notebook;
	GtkWidget *error_info_bar;
	GtkWidget *error_label;
	GtkWidget *results_window;
	GtkWidget *results_notebook;
	GtkUIManager *results_uimanager;
	GtkWidget *results_menu;
	GtkWidget *disk_util_vbox;
	GtkListStore *log_model;
	GtkWidget *log_tree;
	GtkWidget *log_view;
	struct gfio_graphs graphs;
	struct probe_widget probe;
	struct eta_widget eta;
	GtkWidget *page_label;
	gint page_num;
	unsigned int state;

	struct graph *clat_graph;
	struct graph *lat_bucket_graph;

	struct gfio_client *client;
	char *job_file;
	char *host;
	int port;
	int type;
	int server_start;
};

/* Job 완료 결과 - 서버에서 수신한 최종 통계 데이터 */
struct end_results {
	struct group_run_stats gs;
	struct thread_stat ts;
};

/* 클라이언트별 job 옵션 목록 노드 */
struct gfio_client_options {
	struct flist_head list;
	struct thread_options o;
};

/* gfio 클라이언트 - fio 서버 연결과 결과 데이터를 관리하는 구조체 */
struct gfio_client {
	struct gui_entry *ge;
	struct fio_client *client;
	GtkWidget *err_entry;
	uint32_t client_cpus;
	uint64_t client_flags;

	struct flist_head o_list;
	unsigned int o_list_nr;

	struct end_results *results;
	unsigned int nr_results;

	uint32_t update_job_status;
	volatile uint32_t update_job_done;

	struct cmd_du_pdu *du;
	unsigned int nr_du;
};

/* gfio에서 사용하는 MIME 타입 - 드래그 앤 드롭 및 파일 연결에 사용 */
#define GFIO_MIME	"text/fio"

extern void gfio_view_log(struct gui *ui);
extern void gfio_set_state(struct gui_entry *ge, unsigned int state);
extern void clear_ge_ui_info(struct gui_entry *ge);

extern const char *gfio_graph_font;
extern GdkColor gfio_color_white;
extern GdkColor gfio_color_lightyellow;

#endif
