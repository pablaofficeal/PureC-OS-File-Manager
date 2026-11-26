#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fm.h"

#define BAR_H 26
#define PAD_X 10

struct mitem {
    const char *label;
    int act;
    const char *shortcut;
};

struct mdef {
    const char *title;
    int alt_key;
    const struct mitem *items;
    int count;
};

static const struct mitem m_file[] = {
    {"New Folder", A_NEWFOLDER, "Ctrl+Shift+N"},
    {"New File", A_NEWFILE, NULL},
    {NULL, 0, NULL},
    {"Open", A_OPEN, "Enter"},
    {"Open in Terminal", A_OPEN_TERM, NULL},
    {"Properties", A_PROPS, "Alt+Enter"},
    {NULL, 0, NULL},
    {"Close Window", A_CLOSE, "Ctrl+W"},
};

static const struct mitem m_edit[] = {
    {"Cut", A_CUT, "Ctrl+X"},
    {"Copy", A_COPY, "Ctrl+C"},
    {"Paste", A_PASTE, "Ctrl+V"},
    {NULL, 0, NULL},
    {"Rename", A_RENAME, "F2"},
    {"Delete", A_DELETE, "Del"},
    {NULL, 0, NULL},
    {"Select All", A_SELALL, "Ctrl+A"},
};

static const struct mitem m_view[] = {
    {"List", A_LIST, NULL},
    {"Grid", A_GRID, NULL},
    {NULL, 0, NULL},
    {"Show hidden files", A_HIDDEN, "Ctrl+H"},
    {NULL, 0, NULL},
    {"Sort by Name", A_SORT_NAME, NULL},
    {"Sort by Size", A_SORT_SIZE, NULL},
    {"Sort by Modified", A_SORT_DATE, NULL},
    {"Sort by Type", A_SORT_TYPE, NULL},
    {NULL, 0, NULL},
    {"Refresh", A_REFRESH, "F5"},
};

static const struct mitem m_go[] = {
    {"Back", A_BACK, "Alt+Left"},
    {"Forward", A_FWD, "Alt+Right"},
    {"Up", A_UP, "Alt+Up"},
    {NULL, 0, NULL},
    {"Home", A_HOME, NULL},
    {"Enter path", A_ENTERPATH, "Ctrl+L"},
};

static const struct mitem m_help[] = {
    {"About", A_ABOUT, NULL},
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const struct mdef defs[] = {
    {"File", 0x09, m_file, COUNT(m_file)},
    {"Edit", 0x08, m_edit, COUNT(m_edit)},
    {"View", 0x19, m_view, COUNT(m_view)},
    {"Go", 0x0A, m_go, COUNT(m_go)},
    {"Help", 0x0B, m_help, COUNT(m_help)},
};

#define NDEFS COUNT(defs)

static struct pg_menu *cur;
static int cur_idx = -1;
static int hot = -1;

bool fm_action_enabled(int action)
{
    int sel = view_count_selected();
    switch (action) {
    case A_OPEN:
    case A_CUT:
    case A_COPY:
    case A_DELETE:
        return sel > 0;
    case A_RENAME:
        return sel == 1;
    case A_PASTE:
    case A_PASTE_INTO:
        return fm.clip_n > 0;
    case A_BACK:
        return fm.hist_pos > 0;
    case A_FWD:
        return fm.hist_pos + 1 < fm.hist_n;
    case A_UP:
        return strcmp(fm.path, "/") != 0;
    default:
        return true;
    }
}

static void goto_home(void)
{
    const char *home = getenv("HOME");
    fm_navigate(home && *home ? home : "/", true);
}

static void set_sort(int col)
{
    if (fm.sort_col == col)
        fm.sort_asc = !fm.sort_asc;
    else {
        fm.sort_col = col;
        fm.sort_asc = true;
    }
    fm_resort();
}

void fm_do(int action)
{
    switch (action) {
    case A_OPEN: {
        if (view_count_selected() > 1) {
            for (int i = 0; i < fm.nvis; i++)
                if (fm.ents[fm.vis[i]].sel && !fm.ents[fm.vis[i]].dir)
                    open_entry(fm.vis[i]);
        } else {
            int i = view_selected_index();
            if (i >= 0)
                open_entry(i);
        }
        break;
    }
    case A_OPEN_WITH: {
        int i = view_selected_index();
        if (i >= 0) {
            char *p = view_path_of(i);
            if (p)
                dlg_open_with(p);
            free(p);
        }
        break;
    }
    case A_OPEN_TERM: {
        int i = view_selected_index();
        if (i >= 0 && view_count_selected() == 1 && fm.ents[i].dir) {
            char *p = view_path_of(i);
            if (p)
                open_terminal_in(p);
            free(p);
        } else
            open_terminal_in(fm.path);
        break;
    }
    case A_CUT: ops_copy_selection(true); break;
    case A_COPY: ops_copy_selection(false); break;
    case A_PASTE: ops_paste(); break;
    case A_PASTE_INTO: {
        int i = view_selected_index();
        if (i >= 0 && fm.ents[i].dir) {
            char *p = view_path_of(i);
            if (p)
                ops_paste_to(p);
            free(p);
        } else
            ops_paste();
        break;
    }
    case A_RENAME: ops_rename(); break;
    case A_DELETE: ops_delete_selection(); break;
    case A_PROPS: ops_properties(); break;
    case A_NEWFOLDER: ops_new_folder(); break;
    case A_NEWFILE: ops_new_file(); break;
    case A_REFRESH: fm_reload(); break;
    case A_SELALL: view_select_all(); break;
    case A_HIDDEN:
        fm_toggle_hidden();
        hidden_toggle_sync();
        break;
    case A_LIST: fm_set_view(0); break;
    case A_GRID: fm_set_view(1); break;
    case A_SORT_NAME: set_sort(SORT_NAME); break;
    case A_SORT_SIZE: set_sort(SORT_SIZE); break;
    case A_SORT_DATE: set_sort(SORT_DATE); break;
    case A_SORT_TYPE: set_sort(SORT_TYPE); break;
    case A_BACK: fm_go_back(); break;
    case A_FWD: fm_go_forward(); break;
    case A_UP: fm_go_up(); break;
    case A_HOME: goto_home(); break;
    case A_ENTERPATH: fm_focus_path(); break;
    case A_CLOSE: pg_window_close(fm.win); break;
    case A_ABOUT:
        dlg_message("About Files", "Files - the PureC OS file manager.\nList and grid views, copy and move with progress,\nproperties with permissions.");
        break;
    default: break;
    }
}

static bool item_checked(int act)
{
    switch (act) {
    case A_LIST: return !fm.grid;
    case A_GRID: return fm.grid;
    case A_HIDDEN: return fm.show_hidden;
    case A_SORT_NAME: return fm.sort_col == SORT_NAME;
    case A_SORT_SIZE: return fm.sort_col == SORT_SIZE;
    case A_SORT_DATE: return fm.sort_col == SORT_DATE;
    case A_SORT_TYPE: return fm.sort_col == SORT_TYPE;
    default: return false;
    }
}

static void item_cb(int index, void *data)
{
    (void)index;
    cur_idx = -1;
    fm_do((int)(intptr_t)data);
    pg_widget_queue_draw(fm.menubar);
}

static void open_menu(int idx);

static void nav_cb(int dir, void *data)
{
    (void)data;
    if (cur_idx < 0)
        return;
    open_menu((cur_idx + dir + (int)NDEFS) % (int)NDEFS);
}

static int title_x(int i)
{
    int x = fm.menubar->rect.x + 6;
    for (int k = 0; k < i; k++)
        x += pgc_text_w(PGF_BODY, defs[k].title) + PAD_X * 2;
    return x;
}

static int title_at(int px)
{
    for (int i = 0; i < (int)NDEFS; i++) {
        int x0 = title_x(i);
        int x1 = x0 + pgc_text_w(PGF_BODY, defs[i].title) + PAD_X * 2;
        if (px >= x0 && px < x1)
            return i;
    }
    return -1;
}

static void open_menu(int idx)
{
    if (cur) {
        pg_menu_destroy(cur);
        cur = NULL;
    }
    cur_idx = -1;
    pg_widget_queue_draw(fm.menubar);
    struct pg_menu *m = pg_menu_new();
    if (!m)
        return;
    const struct mdef *d = &defs[idx];
    for (int i = 0; i < d->count; i++) {
        const struct mitem *it = &d->items[i];
        if (!it->label) {
            pg_menu_add_separator(m);
            continue;
        }
        int k = pg_menu_add(m, it->label, PG_NO_ICON, item_cb, (void *)(intptr_t)it->act);
        if (it->shortcut)
            pg_menu_set_shortcut(m, k, it->shortcut);
        pg_menu_set_enabled(m, k, fm_action_enabled(it->act));
        if (it->act == A_LIST || it->act == A_GRID || it->act == A_HIDDEN || (it->act >= A_SORT_NAME && it->act <= A_SORT_TYPE))
            pg_menu_set_checked(m, k, item_checked(it->act));
    }
    pg_menu_on_nav(m, nav_cb, NULL);
    cur = m;
    cur_idx = idx;
    pg_menu_popup(m, fm.win, title_x(idx), fm.menubar->rect.y + fm.menubar->rect.h);
}

void menubar_close(void)
{
    if (cur)
        pg_menu_close(cur);
    cur_idx = -1;
    if (fm.menubar)
        pg_widget_queue_draw(fm.menubar);
}

static bool open_now(void)
{
    if (cur && cur_idx >= 0 && !pg_menu_is_open(cur))
        cur_idx = -1;
    return cur_idx >= 0;
}

static void bar_draw(struct pg_widget *w, struct pg_canvas *c, void *data)
{
    (void)data;
    const struct pg_theme *t = pg_theme();
    pgc_fill_a(c, w->rect.x, w->rect.y, w->rect.w, w->rect.h, t->surface, 255);
    bool open = open_now();
    int fh = pgc_font_h(PGF_BODY);
    for (int i = 0; i < (int)NDEFS; i++) {
        int x0 = title_x(i);
        int tw = pgc_text_w(PGF_BODY, defs[i].title);
        bool lit = (open && cur_idx == i) || (!open && hot == i);
        if (lit)
            pgc_rrect(c, x0, w->rect.y + 2, tw + PAD_X * 2, w->rect.h - 4, 5, t->surface3, 255);
        pgc_text(c, PGF_BODY, x0 + PAD_X, w->rect.y + (w->rect.h - fh) / 2, defs[i].title, lit ? t->text : t->text2);
    }
}

static bool bar_input(struct pg_widget *w, const struct pg_event *e, void *data)
{
    (void)data;
    (void)w;
    switch (e->type) {
    case PG_EVENT_POINTER_MOVE: {
        int i = title_at(e->x);
        if (open_now()) {
            if (i >= 0 && i != cur_idx)
                open_menu(i);
        } else if (i != hot) {
            hot = i;
            pg_widget_queue_draw(fm.menubar);
        }
        return true;
    }
    case PG_EVENT_LEAVE:
        if (hot != -1) {
            hot = -1;
            pg_widget_queue_draw(fm.menubar);
        }
        return false;
    case PG_EVENT_POINTER_DOWN: {
        if (e->button != PG_BUTTON_LEFT)
            return false;
        int i = title_at(e->x);
        if (i < 0)
            return true;
        if (open_now() && cur_idx == i)
            menubar_close();
        else
            open_menu(i);
        return true;
    }
    case PG_EVENT_POINTER_UP:
        return true;
    default:
        return false;
    }
}

struct pg_widget *menubar_new(void)
{
    struct pg_widget *w = pg_drawing_new(200, BAR_H, bar_draw, bar_input, NULL);
    if (w) {
        pg_widget_set_expand(w, true, false);
        fm.menubar = w;
    }
    return w;
}

bool menubar_key(const struct pg_event *e)
{
    bool alt = e->mods & PG_MOD_ALT;
    if (alt && !(e->mods & PG_MOD_CTRL)) {
        for (int i = 0; i < (int)NDEFS; i++)
            if (e->key == defs[i].alt_key) {
                open_menu(i);
                return true;
            }
    }
    if (e->key == 0x43 && !alt) {
        open_menu(0);
        return true;
    }
    return false;
}

static bool tog_hot;

static void tog_draw(struct pg_widget *w, struct pg_canvas *c, void *data)
{
    (void)data;
    const struct pg_theme *t = pg_theme();
    int x = w->rect.x, y = w->rect.y, s = w->rect.w < w->rect.h ? w->rect.w : w->rect.h;
    y += (w->rect.h - s) / 2;
    if (fm.show_hidden)
        pgc_rrect(c, x, y, s, s, 8, t->accent, 70);
    else if (tog_hot)
        pgc_rrect(c, x, y, s, s, 8, t->surface3, 255);
    int px = pgc_icon_px(PG_ICON_S);
    pgc_icon(c, fm.show_hidden ? PGI_LOCK_OPEN_VARIANT : PGI_LOCK, PG_ICON_S, x + (s - px) / 2, y + (s - px) / 2, fm.show_hidden ? t->accent : t->text2);
}

static bool tog_input(struct pg_widget *w, const struct pg_event *e, void *data)
{
    (void)data;
    switch (e->type) {
    case PG_EVENT_ENTER:
    case PG_EVENT_POINTER_MOVE:
        if (!tog_hot) {
            tog_hot = true;
            fm_toast("Show hidden files (Ctrl+H)");
            pg_widget_queue_draw(w);
        }
        return true;
    case PG_EVENT_LEAVE:
        tog_hot = false;
        fm_toast("");
        pg_widget_queue_draw(w);
        return false;
    case PG_EVENT_POINTER_DOWN:
        return e->button == PG_BUTTON_LEFT;
    case PG_EVENT_POINTER_UP:
        if (e->button == PG_BUTTON_LEFT)
            fm_do(A_HIDDEN);
        return true;
    default:
        return false;
    }
}

struct pg_widget *hidden_toggle_new(void)
{
    struct pg_widget *w = pg_drawing_new(34, 34, tog_draw, tog_input, NULL);
    if (w) {
        pg_widget_set_expand(w, false, false);
        fm.btn_hidden = w;
    }
    return w;
}

void hidden_toggle_sync(void)
{
    if (fm.btn_hidden)
        pg_widget_queue_draw(fm.btn_hidden);
}
