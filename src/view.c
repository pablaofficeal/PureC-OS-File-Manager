#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "fm.h"

#define HDR 28
#define ROW 30
#define CELL_W 112
#define CELL_H 108
#define SB 10

static int hover = -1;
static int hover_hdr = -1;
static bool rubber, rubber_moved;
static int rb_x0, rb_y0, rb_x1, rb_y1;
static bool sb_drag;
static int sb_off;
static bool down_on_item;
static struct pg_menu *menu;

void view_redraw(void)
{
    if (fm.view)
        pg_widget_queue_draw(fm.view);
}

static int grid_cols(void)
{
    int c = (fm.view->rect.w - SB - 12) / CELL_W;
    return c < 1 ? 1 : c;
}

static int area_top(void)
{
    return fm.view->rect.y + (fm.grid ? 0 : HDR);
}

static int area_h(void)
{
    return fm.view->rect.h - (fm.grid ? 0 : HDR);
}

static int total_h(void)
{
    if (fm.grid)
        return ((fm.nvis + grid_cols() - 1) / grid_cols()) * CELL_H + 12;
    return fm.nvis * ROW + 8;
}

static int max_scroll(void)
{
    int m = total_h() - area_h();
    return m < 0 ? 0 : m;
}

static void clamp_scroll(void)
{
    int m = max_scroll();
    if (fm.scroll > m)
        fm.scroll = m;
    if (fm.scroll < 0)
        fm.scroll = 0;
}

static struct pg_rect item_rect(int i)
{
    struct pg_rect r;
    if (fm.grid) {
        int cols = grid_cols();
        int left = fm.view->rect.x + 6 + (fm.view->rect.w - SB - 12 - cols * CELL_W) / 2;
        r.x = left + (i % cols) * CELL_W;
        r.y = fm.view->rect.y + 6 + (i / cols) * CELL_H - fm.scroll;
        r.w = CELL_W;
        r.h = CELL_H;
    } else {
        r.x = fm.view->rect.x + 4;
        r.y = area_top() + 4 + i * ROW - fm.scroll;
        r.w = fm.view->rect.w - 8 - SB;
        r.h = ROW;
    }
    return r;
}

static int hit_item(int x, int y)
{
    if (y < area_top() || x >= fm.view->rect.x + fm.view->rect.w - SB)
        return -1;
    if (fm.grid) {
        int cols = grid_cols();
        int left = fm.view->rect.x + 6 + (fm.view->rect.w - SB - 12 - cols * CELL_W) / 2;
        int cx = x - left, cy = y - fm.view->rect.y - 6 + fm.scroll;
        if (cx < 0 || cy < 0 || cx >= cols * CELL_W)
            return -1;
        int i = (cy / CELL_H) * cols + cx / CELL_W;
        return i < fm.nvis ? i : -1;
    }
    int cy = y - area_top() - 4 + fm.scroll;
    if (cy < 0)
        return -1;
    int i = cy / ROW;
    return i < fm.nvis ? i : -1;
}

struct cols {
    int size_x, size_w, date_x, date_w, type_x, type_w, name_w;
    bool size, date, type;
};

static struct cols list_cols(void)
{
    struct cols c = {0};
    int right = fm.view->rect.x + fm.view->rect.w - SB - 12;
    int w = fm.view->rect.w;
    int x = right;
    c.type = w >= 780;
    c.date = w >= 560;
    c.size = w >= 380;
    if (c.type) {
        c.type_w = 140;
        x -= c.type_w;
        c.type_x = x;
    }
    if (c.date) {
        c.date_w = 140;
        x -= c.date_w;
        c.date_x = x;
    }
    if (c.size) {
        c.size_w = 84;
        x -= c.size_w;
        c.size_x = x;
    }
    c.name_w = x - (fm.view->rect.x + 12);
    return c;
}

static int hit_header(int x, int y)
{
    if (fm.grid || y < fm.view->rect.y || y >= fm.view->rect.y + HDR)
        return -1;
    struct cols c = list_cols();
    if (c.type && x >= c.type_x)
        return SORT_TYPE;
    if (c.date && x >= c.date_x)
        return SORT_DATE;
    if (c.size && x >= c.size_x)
        return SORT_SIZE;
    return SORT_NAME;
}

int view_count_selected(void)
{
    int n = 0;
    for (int i = 0; i < fm.nvis; i++)
        if (fm.ents[fm.vis[i]].sel)
            n++;
    return n;
}

void view_clear_select(void)
{
    for (int i = 0; i < fm.n; i++)
        fm.ents[i].sel = false;
}

void view_select_all(void)
{
    for (int i = 0; i < fm.nvis; i++)
        fm.ents[fm.vis[i]].sel = true;
    fm_status();
    view_redraw();
}

int view_selected_index(void)
{
    for (int i = 0; i < fm.nvis; i++)
        if (fm.ents[fm.vis[i]].sel)
            return fm.vis[i];
    return fm.cursor >= 0 && fm.cursor < fm.n ? fm.cursor : -1;
}

void view_ensure_visible(void)
{
    if (!fm.view || fm.cursor < 0 || !fm.vpos || fm.cursor >= fm.n || fm.vpos[fm.cursor] < 0) {
        clamp_scroll();
        return;
    }
    int i = fm.vpos[fm.cursor];
    int top, h;
    if (fm.grid) {
        top = 6 + (i / grid_cols()) * CELL_H;
        h = CELL_H;
    } else {
        top = 4 + i * ROW;
        h = ROW;
    }
    if (top < fm.scroll)
        fm.scroll = top;
    else if (top + h > fm.scroll + area_h())
        fm.scroll = top + h - area_h();
    clamp_scroll();
}

static void select_range(int a, int b)
{
    if (a > b) {
        int t = a;
        a = b;
        b = t;
    }
    view_clear_select();
    for (int i = a; i <= b && i < fm.nvis; i++)
        if (i >= 0)
            fm.ents[fm.vis[i]].sel = true;
}

void view_set_cursor(int vi, bool extend, bool keep)
{
    if (vi < 0 || vi >= fm.nvis)
        return;
    int ent = fm.vis[vi];
    if (extend) {
        int a = fm.anchor >= 0 && fm.vpos[fm.anchor] >= 0 ? fm.vpos[fm.anchor] : vi;
        select_range(a, vi);
    } else if (!keep) {
        view_clear_select();
        fm.ents[ent].sel = true;
        fm.anchor = ent;
    }
    fm.cursor = ent;
    view_ensure_visible();
    fm_status();
    view_redraw();
}

int view_page_rows(void)
{
    int r = fm.grid ? (area_h() / CELL_H) * grid_cols() : area_h() / ROW;
    return r < 1 ? 1 : r;
}

char *view_path_of(int ent)
{
    char full[FM_PATH];
    join_path(full, sizeof(full), fm.path, fm.ents[ent].name);
    return strdup(full);
}

char **view_selection_paths(int *count)
{
    int n = view_count_selected();
    *count = 0;
    if (n == 0)
        return NULL;
    char **out = calloc((size_t)n, sizeof(char *));
    if (!out)
        return NULL;
    for (int i = 0; i < fm.nvis; i++)
        if (fm.ents[fm.vis[i]].sel) {
            char *p = view_path_of(fm.vis[i]);
            if (p)
                out[(*count)++] = p;
        }
    return out;
}

void view_free_paths(char **paths, int count)
{
    for (int i = 0; i < count; i++)
        free(paths[i]);
    free(paths);
}

static void sort_arrow(struct pg_canvas *c, int x, int y, bool asc, uint32_t col)
{
    float fx = (float)x, fy = (float)y;
    if (asc) {
        pgc_line(c, fx - 4, fy + 2, fx, fy - 2, 1.4f, col);
        pgc_line(c, fx, fy - 2, fx + 4, fy + 2, 1.4f, col);
    } else {
        pgc_line(c, fx - 4, fy - 2, fx, fy + 2, 1.4f, col);
        pgc_line(c, fx, fy + 2, fx + 4, fy - 2, 1.4f, col);
    }
}

static void draw_header(struct pg_canvas *c)
{
    const struct pg_theme *t = pg_theme();
    struct pg_rect v = fm.view->rect;
    pgc_fill(c, v.x, v.y, v.w, HDR, t->surface);
    pgc_fill(c, v.x, v.y + HDR - 1, v.w, 1, t->border);
    struct cols cl = list_cols();
    int ty = v.y + (HDR - pgc_font_h(PGF_SMALL)) / 2;
    struct {
        int col, x, w;
        const char *label;
        bool show;
    } hs[] = {
        {SORT_NAME, v.x + 12 + 32, cl.name_w - 32, "Name", true},
        {SORT_SIZE, cl.size_x, cl.size_w, "Size", cl.size},
        {SORT_DATE, cl.date_x, cl.date_w, "Modified", cl.date},
        {SORT_TYPE, cl.type_x, cl.type_w, "Type", cl.type},
    };
    for (int i = 0; i < 4; i++) {
        if (!hs[i].show)
            continue;
        bool on = fm.sort_col == hs[i].col;
        if (hover_hdr == hs[i].col)
            pgc_fill_a(c, hs[i].col == SORT_NAME ? v.x : hs[i].x - 6, v.y, hs[i].col == SORT_NAME ? hs[i].x + hs[i].w - v.x : hs[i].w + 6, HDR - 1, t->surface3, 120);
        int tw = pgc_text(c, PGF_SMALL, hs[i].x, ty, hs[i].label, on ? t->text : t->text2);
        if (on)
            sort_arrow(c, hs[i].x + tw + 10, v.y + HDR / 2, fm.sort_asc, t->accent);
    }
}

static void draw_list_row(struct pg_canvas *c, int vi, struct cols *cl)
{
    const struct pg_theme *t = pg_theme();
    struct entry *e = &fm.ents[fm.vis[vi]];
    struct pg_rect r = item_rect(vi);
    if (r.y + r.h < area_top() || r.y > fm.view->rect.y + fm.view->rect.h)
        return;
    bool cur = fm.vis[vi] == fm.cursor && pg_widget_has_focus(fm.view);
    if (e->sel)
        pgc_rrect(c, r.x, r.y, r.w, r.h - 2, 7, t->accent, 70);
    else if (vi == hover)
        pgc_rrect(c, r.x, r.y, r.w, r.h - 2, 7, t->surface2, 255);
    if (cur)
        pgc_rrect_stroke(c, r.x, r.y, r.w, r.h - 2, 7, t->accent, 255);
    int ty = r.y + (r.h - 2 - pgc_font_h(PGF_BODY)) / 2;
    icon_draw(c, e->stated ? e->kind : e->dir ? IK_FOLDER : IK_FILE, e->link, r.x + 8, r.y + (r.h - 2 - 20) / 2, 20);
    uint32_t col = e->hidden ? t->text3 : t->text;
    pgc_text_fit(c, PGF_BODY, r.x + 40, ty, cl->name_w - 32, e->name, col);
    char buf[64];
    if (cl->size && e->stated && !e->dir) {
        format_size(e->size, buf, sizeof(buf));
        pgc_text(c, PGF_BODY, cl->size_x + cl->size_w - 12 - pgc_text_w(PGF_BODY, buf), ty, buf, t->text2);
    }
    if (cl->date && e->stated) {
        format_time(e->mtime, buf, sizeof(buf));
        pgc_text_fit(c, PGF_BODY, cl->date_x, ty, cl->date_w, buf, t->text2);
    }
    if (cl->type && e->stated)
        pgc_text_fit(c, PGF_BODY, cl->type_x, ty, cl->type_w - 6, type_name_for(e), t->text2);
}

static void draw_grid_cell(struct pg_canvas *c, int vi)
{
    const struct pg_theme *t = pg_theme();
    struct entry *e = &fm.ents[fm.vis[vi]];
    struct pg_rect r = item_rect(vi);
    if (r.y + r.h < fm.view->rect.y || r.y > fm.view->rect.y + fm.view->rect.h)
        return;
    bool cur = fm.vis[vi] == fm.cursor && pg_widget_has_focus(fm.view);
    if (e->sel)
        pgc_rrect(c, r.x + 3, r.y + 2, r.w - 6, r.h - 6, 9, t->accent, 70);
    else if (vi == hover)
        pgc_rrect(c, r.x + 3, r.y + 2, r.w - 6, r.h - 6, 9, t->surface2, 255);
    if (cur)
        pgc_rrect_stroke(c, r.x + 3, r.y + 2, r.w - 6, r.h - 6, 9, t->accent, 255);
    icon_draw(c, e->stated ? e->kind : e->dir ? IK_FOLDER : IK_FILE, e->link, r.x + (r.w - 52) / 2, r.y + 10, 52);
    uint32_t col = e->hidden ? t->text3 : t->text;
    int fh = pgc_font_h(PGF_BODY);
    int tw = pgc_text_w(PGF_BODY, e->name);
    int maxw = r.w - 14;
    if (tw <= maxw) {
        pgc_text(c, PGF_BODY, r.x + (r.w - tw) / 2, r.y + 70, e->name, col);
        return;
    }
    char first[256], rest[256];
    size_t len = strlen(e->name), cut = len;
    while (cut > 1) {
        snprintf(first, sizeof(first), "%.*s", (int)cut, e->name);
        if (pgc_text_w(PGF_BODY, first) <= maxw)
            break;
        cut--;
        while (cut > 1 && ((unsigned char)e->name[cut] & 0xC0) == 0x80)
            cut--;
    }
    snprintf(first, sizeof(first), "%.*s", (int)cut, e->name);
    snprintf(rest, sizeof(rest), "%s", e->name + cut);
    pgc_text(c, PGF_BODY, r.x + (r.w - pgc_text_w(PGF_BODY, first)) / 2, r.y + 70 - fh / 3, first, col);
    int rw = pgc_text_w(PGF_BODY, rest);
    if (rw <= maxw)
        pgc_text(c, PGF_BODY, r.x + (r.w - rw) / 2, r.y + 70 + fh - fh / 3, rest, col);
    else
        pgc_text_fit(c, PGF_BODY, r.x + 7, r.y + 70 + fh - fh / 3, maxw, rest, col);
}

static void draw_scrollbar(struct pg_canvas *c)
{
    const struct pg_theme *t = pg_theme();
    int th = total_h(), ah = area_h();
    if (th <= ah)
        return;
    int track = ah - 4;
    int len = track * ah / th;
    if (len < 28)
        len = 28;
    int y = area_top() + 2 + (track - len) * fm.scroll / (th - ah);
    bool hot = sb_drag || fm.view->hovered;
    pgc_rrect(c, fm.view->rect.x + fm.view->rect.w - SB + 2, y, 5, len, 3, hot ? t->text3 : t->surface3, hot ? 220 : 170);
}

static void view_draw(struct pg_widget *w, struct pg_canvas *c, void *data)
{
    (void)data;
    const struct pg_theme *t = pg_theme();
    struct pg_rect v = w->rect;
    pgc_fill(c, v.x, v.y, v.w, v.h, t->bg);
    clamp_scroll();
    struct pg_clip saved = pgc_clip_push(c, v.x, area_top(), v.w, area_h());
    if (fm.nvis == 0) {
        const char *msg = fm.loading ? "Loading..." : fm.filter_text[0] ? "No matches" : "This folder is empty";
        int tw = pgc_text_w(PGF_MEDIUM, msg);
        pgc_text(c, PGF_MEDIUM, v.x + (v.w - tw) / 2, area_top() + area_h() / 2 - 10, msg, t->text3);
    } else if (fm.grid) {
        int cols = grid_cols();
        int first = fm.scroll / CELL_H * cols;
        int last = ((fm.scroll + area_h()) / CELL_H + 1) * cols + cols;
        for (int i = first < 0 ? 0 : first; i < fm.nvis && i < last; i++)
            draw_grid_cell(c, i);
    } else {
        struct cols cl = list_cols();
        int first = fm.scroll / ROW - 1, last = (fm.scroll + area_h()) / ROW + 2;
        for (int i = first < 0 ? 0 : first; i < fm.nvis && i < last; i++)
            draw_list_row(c, i, &cl);
    }
    if (rubber && rubber_moved) {
        int x0 = rb_x0 < rb_x1 ? rb_x0 : rb_x1, x1 = rb_x0 < rb_x1 ? rb_x1 : rb_x0;
        int y0 = rb_y0 < rb_y1 ? rb_y0 : rb_y1, y1 = rb_y0 < rb_y1 ? rb_y1 : rb_y0;
        int sy0 = y0 - fm.scroll + (fm.grid ? 0 : 0), sy1 = y1 - fm.scroll;
        pgc_fill_a(c, x0, sy0, x1 - x0, sy1 - sy0, t->accent, 50);
        pgc_rrect_stroke(c, x0, sy0, x1 - x0, sy1 - sy0, 0, t->accent, 255);
    }
    pgc_clip_pop(c, saved);
    if (!fm.grid)
        draw_header(c);
    draw_scrollbar(c);
}

static void rubber_apply(void)
{
    int x0 = rb_x0 < rb_x1 ? rb_x0 : rb_x1, x1 = rb_x0 < rb_x1 ? rb_x1 : rb_x0;
    int y0 = rb_y0 < rb_y1 ? rb_y0 : rb_y1, y1 = rb_y0 < rb_y1 ? rb_y1 : rb_y0;
    view_clear_select();
    int saved = fm.scroll;
    fm.scroll = 0;
    for (int i = 0; i < fm.nvis; i++) {
        struct pg_rect r = item_rect(i);
        bool hit;
        if (fm.grid)
            hit = r.x + 6 < x1 && r.x + r.w - 6 > x0 && r.y < y1 && r.y + r.h - 4 > y0;
        else
            hit = r.y < y1 && r.y + r.h > y0;
        if (hit)
            fm.ents[fm.vis[i]].sel = true;
    }
    fm.scroll = saved;
    fm_status();
}

void view_rubber_reset(void)
{
    rubber = false;
}

static void menu_cb(int index, void *data)
{
    (void)index;
    fm_do((int)(intptr_t)data);
}

static void ctx_add(const char *label, int act, const char *shortcut, bool enabled, bool checked)
{
    int k = pg_menu_add(menu, label, PG_NO_ICON, menu_cb, (void *)(intptr_t)act);
    if (shortcut)
        pg_menu_set_shortcut(menu, k, shortcut);
    pg_menu_set_enabled(menu, k, enabled);
    if (act == A_HIDDEN)
        pg_menu_set_checked(menu, k, checked);
}

void view_context(int x, int y, bool on_item)
{
    if (menu)
        pg_menu_destroy(menu);
    menu = pg_menu_new();
    if (!menu)
        return;
    struct pg_window *win = fm.win;
    int wx = x - 0, wy = y - 0;
    if (on_item) {
        int n = view_count_selected();
        int si = view_selected_index();
        bool one_dir = n == 1 && si >= 0 && fm.ents[si].dir;
        ctx_add("Open", A_OPEN, "Enter", true, false);
        if (n == 1)
            ctx_add("Open with...", A_OPEN_WITH, NULL, true, false);
        if (one_dir)
            ctx_add("Open in Terminal", A_OPEN_TERM, NULL, true, false);
        pg_menu_add_separator(menu);
        ctx_add("Cut", A_CUT, "Ctrl+X", true, false);
        ctx_add("Copy", A_COPY, "Ctrl+C", true, false);
        ctx_add(one_dir ? "Paste into" : "Paste", one_dir ? A_PASTE_INTO : A_PASTE, "Ctrl+V", fm.clip_n > 0, false);
        pg_menu_add_separator(menu);
        ctx_add("Rename", A_RENAME, "F2", n == 1, false);
        ctx_add("Delete", A_DELETE, "Del", true, false);
        pg_menu_add_separator(menu);
        ctx_add("Properties", A_PROPS, "Alt+Enter", true, false);
    } else {
        ctx_add("New Folder", A_NEWFOLDER, "Ctrl+Shift+N", true, false);
        ctx_add("New File", A_NEWFILE, NULL, true, false);
        ctx_add("Paste", A_PASTE, "Ctrl+V", fm.clip_n > 0, false);
        pg_menu_add_separator(menu);
        ctx_add("Select All", A_SELALL, "Ctrl+A", true, false);
        ctx_add("Show hidden files", A_HIDDEN, "Ctrl+H", true, fm.show_hidden);
        ctx_add("Refresh", A_REFRESH, "F5", true, false);
        pg_menu_add_separator(menu);
        ctx_add("Properties", A_PROPS, "Alt+Enter", true, false);
    }
    pg_menu_popup(menu, win, wx, wy);
}

static void sb_set_from_y(int y)
{
    int th = total_h(), ah = area_h();
    int track = ah - 4;
    int len = track * ah / th;
    if (len < 28)
        len = 28;
    int span = track - len;
    if (span <= 0)
        return;
    int pos = y - sb_off - (area_top() + 2);
    fm.scroll = pos * (th - ah) / span;
    clamp_scroll();
}

static bool view_input(struct pg_widget *w, const struct pg_event *e, void *data)
{
    (void)data;
    switch (e->type) {
    case PG_EVENT_WHEEL:
        fm.scroll -= e->wheel * (fm.grid ? 60 : ROW * 2);
        clamp_scroll();
        view_redraw();
        return true;
    case PG_EVENT_LEAVE:
        hover = hover_hdr = -1;
        view_redraw();
        return false;
    case PG_EVENT_POINTER_MOVE: {
        if (sb_drag) {
            sb_set_from_y(e->y);
            view_redraw();
            return true;
        }
        if (rubber) {
            if (!rubber_moved && (abs(e->x - rb_x0) > 4 || abs(e->y + fm.scroll - rb_y0) > 4))
                rubber_moved = true;
            if (rubber_moved) {
                rb_x1 = e->x;
                rb_y1 = e->y + fm.scroll;
                if (e->y < area_top() + 4)
                    fm.scroll -= 12;
                else if (e->y > area_top() + area_h() - 4)
                    fm.scroll += 12;
                clamp_scroll();
                rubber_apply();
                view_redraw();
            }
            return true;
        }
        int h = hit_item(e->x, e->y), hh = hit_header(e->x, e->y);
        if (h != hover || hh != hover_hdr) {
            hover = h;
            hover_hdr = hh;
            view_redraw();
        }
        return true;
    }
    case PG_EVENT_POINTER_UP:
        sb_drag = false;
        if (rubber) {
            rubber = false;
            rubber_moved = false;
            fm_status();
            view_redraw();
        }
        return true;
    case PG_EVENT_POINTER_DOWN: {
        pg_widget_grab_focus(w);
        pg_stack_set_index(fm.pathstack, 0);
        if (e->button != PG_BUTTON_LEFT && e->button != PG_BUTTON_RIGHT)
            return false;
        bool ctrl = e->mods & PG_MOD_CTRL, shift = e->mods & PG_MOD_SHIFT;
        int hdr = hit_header(e->x, e->y);
        if (hdr >= 0 && e->button == PG_BUTTON_LEFT) {
            if (fm.sort_col == hdr)
                fm.sort_asc = !fm.sort_asc;
            else {
                fm.sort_col = hdr;
                fm.sort_asc = hdr == SORT_NAME || hdr == SORT_TYPE;
            }
            fm_resort();
            view_ensure_visible();
            return true;
        }
        if (e->button == PG_BUTTON_LEFT && total_h() > area_h() && e->x >= w->rect.x + w->rect.w - SB && e->y >= area_top()) {
            int th = total_h(), ah = area_h(), track = ah - 4;
            int len = track * ah / th;
            if (len < 28)
                len = 28;
            int ty = area_top() + 2 + (track - len) * fm.scroll / (th - ah);
            sb_off = e->y >= ty && e->y < ty + len ? e->y - ty : len / 2;
            sb_drag = true;
            sb_set_from_y(e->y);
            view_redraw();
            return true;
        }
        int it = hit_item(e->x, e->y);
        if (e->button == PG_BUTTON_RIGHT) {
            if (it >= 0 && !fm.ents[fm.vis[it]].sel)
                view_set_cursor(it, false, false);
            else if (it < 0)
                view_clear_select();
            fm_status();
            view_redraw();
            view_context(e->x, e->y, it >= 0);
            return true;
        }
        if (it < 0) {
            if (!ctrl && !shift)
                view_clear_select();
            rubber = true;
            rubber_moved = false;
            rb_x0 = rb_x1 = e->x;
            rb_y0 = rb_y1 = e->y + fm.scroll;
            fm_status();
            view_redraw();
            return true;
        }
        down_on_item = true;
        int ent = fm.vis[it];
        if (e->clicks >= 2 && !ctrl && !shift) {
            view_set_cursor(it, false, false);
            open_entry(ent);
            return true;
        }
        if (shift)
            view_set_cursor(it, true, false);
        else if (ctrl) {
            fm.ents[ent].sel = !fm.ents[ent].sel;
            fm.cursor = fm.anchor = ent;
            fm_status();
            view_redraw();
        } else
            view_set_cursor(it, false, false);
        return true;
    }
    default:
        return false;
    }
}

static void move_cursor(int delta, bool extend, bool keep)
{
    if (!fm.nvis)
        return;
    int cur = fm.cursor >= 0 && fm.vpos[fm.cursor] >= 0 ? fm.vpos[fm.cursor] : -1;
    int next = cur < 0 ? (delta > 0 ? 0 : fm.nvis - 1) : cur + delta;
    if (next < 0)
        next = 0;
    if (next >= fm.nvis)
        next = fm.nvis - 1;
    view_set_cursor(next, extend, keep);
}

static bool type_select(uint32_t ch)
{
    uint64_t now = pg_time_ms();
    if (now - fm.typetime > 900)
        fm.typebuf[0] = 0;
    fm.typetime = now;
    size_t n = strlen(fm.typebuf);
    if (n + 1 < sizeof(fm.typebuf)) {
        fm.typebuf[n] = (char)ch;
        fm.typebuf[n + 1] = 0;
    }
    size_t len = strlen(fm.typebuf);
    int start = fm.cursor >= 0 && fm.vpos[fm.cursor] >= 0 ? fm.vpos[fm.cursor] : 0;
    if (len == 1)
        start++;
    for (int k = 0; k < fm.nvis; k++) {
        int i = (start + k) % fm.nvis;
        if (strncasecmp(fm.ents[fm.vis[i]].name, fm.typebuf, len) == 0) {
            view_set_cursor(i, false, false);
            return true;
        }
    }
    return true;
}

bool view_key(const struct pg_event *e)
{
    bool ctrl = e->mods & PG_MOD_CTRL, shift = e->mods & PG_MOD_SHIFT, alt = e->mods & PG_MOD_ALT;
    if (alt)
        return false;
    int cols = fm.grid ? grid_cols() : 1;
    switch (e->key) {
    case PG_KEY_DOWN: move_cursor(cols, shift, ctrl); return true;
    case PG_KEY_UP: move_cursor(-cols, shift, ctrl); return true;
    case PG_KEY_RIGHT:
        if (!fm.grid)
            return false;
        move_cursor(1, shift, ctrl);
        return true;
    case PG_KEY_LEFT:
        if (!fm.grid)
            return false;
        move_cursor(-1, shift, ctrl);
        return true;
    case PG_KEY_PAGE_DOWN: move_cursor(view_page_rows(), shift, ctrl); return true;
    case PG_KEY_PAGE_UP: move_cursor(-view_page_rows(), shift, ctrl); return true;
    case PG_KEY_HOME:
        view_set_cursor(0, shift, ctrl);
        return true;
    case PG_KEY_END:
        view_set_cursor(fm.nvis - 1, shift, ctrl);
        return true;
    case PG_KEY_ESCAPE:
        view_clear_select();
        fm_status();
        view_redraw();
        return true;
    case PG_KEY_ENTER:
    case PG_KEY_KP_ENTER:
        if (view_count_selected() > 1) {
            for (int i = 0; i < fm.nvis; i++)
                if (fm.ents[fm.vis[i]].sel && !fm.ents[fm.vis[i]].dir)
                    open_entry(fm.vis[i]);
        } else {
            int i = view_selected_index();
            if (i >= 0)
                open_entry(i);
        }
        return true;
    default:
        break;
    }
    if (e->key == PG_KEY_SPACE && !fm.typebuf[0] && fm.cursor >= 0) {
        fm.ents[fm.cursor].sel = !fm.ents[fm.cursor].sel;
        fm_status();
        view_redraw();
        return true;
    }
    if (e->ch && !ctrl)
        return type_select(e->ch);
    return false;
}

struct pg_widget *view_new(void)
{
    struct pg_widget *w = pg_drawing_new(200, 200, view_draw, view_input, NULL);
    if (w) {
        pg_widget_set_expand(w, true, true);
        fm.view = w;
    }
    return w;
}
