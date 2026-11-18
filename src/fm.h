#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pg/pg.h"

#define FM_PATH 1024

enum icon_kind {
    IK_FOLDER = 0,
    IK_FILE,
    IK_TEXT,
    IK_IMAGE,
    IK_ARCHIVE,
    IK_EXEC,
    IK_PACKAGE,
    IK_DRIVE,
    IK_AUDIO,
    IK_CODE
};

struct entry {
    char *name;
    char *type;
    uint64_t size;
    int64_t mtime;
    unsigned mode;
    unsigned uid, gid;
    unsigned char kind;
    bool dir;
    bool link;
    bool hidden;
    bool sel;
    bool stated;
};

enum sort_col { SORT_NAME = 0, SORT_SIZE, SORT_DATE, SORT_TYPE };

struct fm {
    struct pg_window *win;
    struct pg_widget *view;
    struct pg_widget *sidebar;
    struct pg_widget *crumbs;
    struct pg_widget *pathstack;
    struct pg_widget *pathentry;
    struct pg_widget *filter;
    struct pg_widget *btn_back, *btn_fwd, *btn_up, *btn_view;
    struct pg_widget *st_left, *st_right;
    struct pg_widget *st_busy;

    char path[FM_PATH];
    char **hist;
    int hist_n, hist_pos, hist_cap;

    struct entry *ents;
    int n, cap;
    int *vis;
    int *vpos;
    int nvis;
    int load_pos;
    uint32_t load_timer;
    bool loading;
    char err[160];

    int cursor, anchor;
    int scroll;
    int grid;
    int sort_col;
    bool sort_asc;
    bool show_hidden;
    char filter_text[128];

    char **clip;
    int clip_n;
    bool clip_cut;

    char pending_select[256];
    char typebuf[64];
    uint64_t typetime;
};

extern struct fm fm;

bool fm_navigate(const char *path, bool push);
void fm_reload(void);
void fm_go_back(void);
void fm_go_forward(void);
void fm_go_up(void);
void fm_apply_filter(void);
void fm_resort(void);
void fm_status(void);
void fm_toggle_hidden(void);
void fm_set_view(int grid);
void fm_focus_path(void);
void fm_select_name(const char *name);
void fm_sidebar_refresh(void);
void fm_toast(const char *text);

int view_count_selected(void);
int view_selected_index(void);
void view_select_all(void);
void view_clear_select(void);
void view_set_cursor(int vis_index, bool extend, bool keep);
void view_ensure_visible(void);
void view_redraw(void);
void view_context(int x, int y, bool on_item);
bool view_key(const struct pg_event *e);
struct pg_widget *view_new(void);
void view_rubber_reset(void);
char *view_path_of(int ent_index);
char **view_selection_paths(int *count);
void view_free_paths(char **paths, int count);
int view_page_rows(void);

void icon_draw(struct pg_canvas *c, int kind, bool link, int x, int y, int size);
int icon_kind_for(const struct entry *e);
const char *type_name_for(const struct entry *e);

void format_size(uint64_t size, char *out, size_t cap);
void format_time(int64_t t, char *out, size_t cap);
void join_path(char *out, size_t cap, const char *dir, const char *name);
const char *base_name(const char *path);
void parent_path(const char *path, char *out, size_t cap);
const char *ext_of(const char *name);

typedef void (*dlg_done_fn)(const char *text, void *data);
void dlg_message(const char *title, const char *text);
void dlg_confirm(const char *title, const char *text, const char *ok, bool danger, dlg_done_fn fn, void *data);
void dlg_input(const char *title, const char *label, const char *initial, const char *ok, dlg_done_fn fn, void *data);
void dlg_properties(const char *path);
void dlg_open_with(const char *path);

enum conflict_answer { CF_SKIP = 1, CF_REPLACE, CF_KEEP, CF_CANCEL };
void dlg_conflict(const char *name, bool all_possible, void (*answer)(int choice, bool all));
struct pg_window *dlg_progress(const char *title, void (*cancel)(void));
void dlg_progress_update(struct pg_window *w, const char *line, uint32_t permille);

void ops_copy_selection(bool cut);
void ops_paste(void);
void ops_delete_selection(void);
void ops_new_folder(void);
void ops_new_file(void);
void ops_rename(void);
void ops_properties(void);
bool ops_busy(void);
void ops_start_transfer(char **srcs, int n, const char *dest, bool move);
void ops_start_delete(char **srcs, int n);
bool ops_dir_size(const char *path, uint64_t *bytes, uint64_t *files, uint64_t *dirs, volatile bool *cancel);
void ops_clip_clear(void);
void ops_make_unique(const char *dir, const char *name, char *out, size_t cap);
const char *ops_strerror(int err);
void ops_shutdown(void);

void open_entry(int ent_index);
void open_path(const char *path, bool is_dir);
int open_trampoline(const char *cmd, bool pause);
void open_load_config(void);
void open_with_command(const char *cmd, const char *path);
