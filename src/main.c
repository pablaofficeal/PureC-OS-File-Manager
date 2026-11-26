#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
#include "fm.h"

struct fm fm;

#define MAX_PLACES 48

static char *place_path[MAX_PLACES];
static int place_n;
static bool sidebar_lock;
static uint32_t toast_timer;

void join_path(char *out, size_t cap, const char *dir, const char *name)
{
    if (strcmp(dir, "/") == 0)
        snprintf(out, cap, "/%s", name);
    else
        snprintf(out, cap, "%s/%s", dir, name);
}

const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

void parent_path(const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s", path);
    char *s = strrchr(out, '/');
    if (!s || s == out)
        snprintf(out, cap, "/");
    else
        *s = 0;
}

void format_time(int64_t t, char *out, size_t cap)
{
    time_t v = (time_t)t;
    struct tm tmv;
    if (t <= 0 || !localtime_r(&v, &tmv)) {
        snprintf(out, cap, "-");
        return;
    }
    snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
}

static void free_entries(void)
{
    for (int i = 0; i < fm.n; i++) {
        free(fm.ents[i].name);
        free(fm.ents[i].type);
    }
    free(fm.ents);
    free(fm.vis);
    free(fm.vpos);
    fm.ents = NULL;
    fm.vis = NULL;
    fm.vpos = NULL;
    fm.n = fm.cap = fm.nvis = 0;
}

static int cmp_vis(const void *a, const void *b)
{
    const struct entry *x = &fm.ents[*(const int *)a], *y = &fm.ents[*(const int *)b];
    if (x->dir != y->dir)
        return x->dir ? -1 : 1;
    long long r = 0;
    switch (fm.sort_col) {
    case SORT_SIZE:
        r = x->size < y->size ? -1 : x->size > y->size ? 1 : 0;
        break;
    case SORT_DATE:
        r = x->mtime < y->mtime ? -1 : x->mtime > y->mtime ? 1 : 0;
        break;
    case SORT_TYPE: {
        const char *tx = x->type ? x->type : type_name_for(x), *ty = y->type ? y->type : type_name_for(y);
        r = strcasecmp(tx, ty);
        break;
    }
    default:
        break;
    }
    if (r == 0)
        r = strcasecmp(x->name, y->name);
    if (r == 0)
        r = strcmp(x->name, y->name);
    if (!fm.sort_asc)
        r = -r;
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

static void rebuild_vpos(void)
{
    for (int i = 0; i < fm.n; i++)
        fm.vpos[i] = -1;
    for (int i = 0; i < fm.nvis; i++)
        fm.vpos[fm.vis[i]] = i;
}

void fm_resort(void)
{
    if (fm.nvis > 1)
        qsort(fm.vis, (size_t)fm.nvis, sizeof(int), cmp_vis);
    rebuild_vpos();
    view_redraw();
}

void fm_apply_filter(void)
{
    free(fm.vis);
    free(fm.vpos);
    fm.vis = malloc(sizeof(int) * (size_t)(fm.n + 1));
    fm.vpos = malloc(sizeof(int) * (size_t)(fm.n + 1));
    fm.nvis = 0;
    if (!fm.vis || !fm.vpos)
        return;
    for (int i = 0; i < fm.n; i++) {
        struct entry *e = &fm.ents[i];
        if (e->hidden && !fm.show_hidden)
            continue;
        if (fm.filter_text[0] && !strcasestr(e->name, fm.filter_text))
            continue;
        fm.vis[fm.nvis++] = i;
    }
    fm_resort();
    fm_status();
}

static void entry_stat(struct entry *e)
{
    char full[FM_PATH];
    struct stat st;
    join_path(full, sizeof(full), fm.path, e->name);
    if (lstat(full, &st) != 0) {
        e->stated = true;
        return;
    }
    e->link = S_ISLNK(st.st_mode);
    if (e->link) {
        struct stat target;
        if (stat(full, &target) == 0)
            st = target;
    }
    e->dir = S_ISDIR(st.st_mode);
    e->size = e->dir ? 0 : (uint64_t)st.st_size;
    e->mtime = (int64_t)st.st_mtime;
    e->mode = st.st_mode;
    e->uid = st.st_uid;
    e->gid = st.st_gid;
    e->stated = true;
    e->kind = (unsigned char)icon_kind_for(e);
}

static bool load_tick(void *data)
{
    (void)data;
    uint64_t start = pg_time_ms();
    bool changed = false;
    while (fm.load_pos < fm.n) {
        entry_stat(&fm.ents[fm.load_pos++]);
        changed = true;
        if ((fm.load_pos & 15) == 0 && pg_time_ms() - start > 12)
            break;
    }
    if (fm.load_pos >= fm.n) {
        fm.loading = false;
        fm.load_timer = 0;
        fm_apply_filter();
        if (fm.pending_select[0]) {
            fm_select_name(fm.pending_select);
            fm.pending_select[0] = 0;
        }
        return false;
    }
    if (changed) {
        fm_status();
        view_redraw();
    }
    return true;
}

static bool read_dir(void)
{
    DIR *d = opendir(fm.path);
    if (!d) {
        snprintf(fm.err, sizeof(fm.err), "%s", strerror(errno));
        return false;
    }
    free_entries();
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (fm.n == fm.cap) {
            int cap = fm.cap ? fm.cap * 2 : 128;
            struct entry *n = realloc(fm.ents, sizeof(struct entry) * (size_t)cap);
            if (!n)
                break;
            fm.ents = n;
            fm.cap = cap;
        }
        struct entry *e = &fm.ents[fm.n++];
        memset(e, 0, sizeof(*e));
        e->name = strdup(de->d_name);
        e->dir = de->d_type == DT_DIR;
        e->link = de->d_type == DT_LNK;
        e->hidden = de->d_name[0] == '.';
        if (!e->name) {
            fm.n--;
            break;
        }
    }
    closedir(d);
    return true;
}

static void sync_sidebar(void)
{
    int sel = -1;
    for (int i = 0; i < place_n; i++)
        if (strcmp(place_path[i], fm.path) == 0)
            sel = i;
    sidebar_lock = true;
    pg_list_set_selected(fm.sidebar, sel);
    sidebar_lock = false;
}

static void sync_nav_buttons(void)
{
    pg_widget_set_enabled(fm.btn_back, fm.hist_pos > 0);
    pg_widget_set_enabled(fm.btn_fwd, fm.hist_pos + 1 < fm.hist_n);
    pg_widget_set_enabled(fm.btn_up, strcmp(fm.path, "/") != 0);
}

static bool load_current(bool keep_scroll)
{
    if (!read_dir())
        return false;
    fm.cursor = fm.anchor = -1;
    if (!keep_scroll)
        fm.scroll = 0;
    fm.loading = fm.n > 0;
    fm.load_pos = 0;
    fm_apply_filter();
    if (fm.load_timer)
        pg_timer_remove(fm.load_timer);
    fm.load_timer = 0;
    if (fm.loading)
        fm.load_timer = pg_timer_add(1, load_tick, NULL);
    else if (fm.pending_select[0]) {
        fm_select_name(fm.pending_select);
        fm.pending_select[0] = 0;
    }
    char title[FM_PATH + 16];
    snprintf(title, sizeof(title), "Files - %s", strcmp(fm.path, "/") == 0 ? "/" : base_name(fm.path));
    pg_window_set_title(fm.win, title);
    pg_widget_queue_draw(fm.crumbs);
    sync_nav_buttons();
    sync_sidebar();
    return true;
}

static void hist_push(const char *path)
{
    while (fm.hist_n > fm.hist_pos + 1)
        free(fm.hist[--fm.hist_n]);
    if (fm.hist_n && strcmp(fm.hist[fm.hist_n - 1], path) == 0) {
        fm.hist_pos = fm.hist_n - 1;
        return;
    }
    if (fm.hist_n == fm.hist_cap) {
        int cap = fm.hist_cap ? fm.hist_cap * 2 : 32;
        char **n = realloc(fm.hist, sizeof(char *) * (size_t)cap);
        if (!n)
            return;
        fm.hist = n;
        fm.hist_cap = cap;
    }
    fm.hist[fm.hist_n++] = strdup(path);
    fm.hist_pos = fm.hist_n - 1;
}

bool fm_navigate(const char *path, bool push)
{
    char resolved[FM_PATH];
    if (!realpath(path, resolved))
        snprintf(resolved, sizeof(resolved), "%s", path);
    char old[FM_PATH];
    snprintf(old, sizeof(old), "%s", fm.path);
    snprintf(fm.path, sizeof(fm.path), "%s", resolved);
    if (!load_current(false)) {
        snprintf(fm.path, sizeof(fm.path), "%s", old);
        char msg[FM_PATH + 200];
        snprintf(msg, sizeof(msg), "%s\n%s", resolved, fm.err);
        dlg_message("Cannot open folder", msg);
        return false;
    }
    if (push)
        hist_push(fm.path);
    sync_nav_buttons();
    pg_stack_set_index(fm.pathstack, 0);
    return true;
}

void fm_reload(void)
{
    if (fm.loading && fm.load_timer) {
        pg_timer_remove(fm.load_timer);
        fm.load_timer = 0;
    }
    int cur = fm.cursor;
    if (cur >= 0 && cur < fm.n && !fm.pending_select[0])
        snprintf(fm.pending_select, sizeof(fm.pending_select), "%s", fm.ents[cur].name);
    if (!load_current(true)) {
        char up[FM_PATH];
        parent_path(fm.path, up, sizeof(up));
        fm_navigate(up, true);
    }
}

void fm_go_back(void)
{
    if (fm.hist_pos <= 0)
        return;
    char target[FM_PATH];
    snprintf(target, sizeof(target), "%s", fm.hist[fm.hist_pos - 1]);
    int pos = fm.hist_pos - 1;
    if (fm_navigate(target, false))
        fm.hist_pos = pos;
    sync_nav_buttons();
}

void fm_go_forward(void)
{
    if (fm.hist_pos + 1 >= fm.hist_n)
        return;
    char target[FM_PATH];
    snprintf(target, sizeof(target), "%s", fm.hist[fm.hist_pos + 1]);
    int pos = fm.hist_pos + 1;
    if (fm_navigate(target, false))
        fm.hist_pos = pos;
    sync_nav_buttons();
}

void fm_go_up(void)
{
    if (strcmp(fm.path, "/") == 0)
        return;
    char up[FM_PATH], from[256];
    snprintf(from, sizeof(from), "%s", base_name(fm.path));
    parent_path(fm.path, up, sizeof(up));
    if (fm_navigate(up, true))
        snprintf(fm.pending_select, sizeof(fm.pending_select), "%s", from);
    if (!fm.loading && fm.pending_select[0]) {
        fm_select_name(fm.pending_select);
        fm.pending_select[0] = 0;
    }
}

void fm_select_name(const char *name)
{
    for (int i = 0; i < fm.n; i++) {
        if (strcmp(fm.ents[i].name, name) == 0 && fm.vpos && fm.vpos[i] >= 0) {
            view_clear_select();
            view_set_cursor(fm.vpos[i], false, false);
            return;
        }
    }
}

void fm_toggle_hidden(void)
{
    fm.show_hidden = !fm.show_hidden;
    fm_apply_filter();
}

void fm_set_view(int grid)
{
    fm.grid = grid;
    fm.scroll = 0;
    pg_segmented_set_selected(fm.btn_view, grid);
    view_ensure_visible();
    view_redraw();
}

void fm_focus_path(void)
{
    pg_stack_set_index(fm.pathstack, 1);
    pg_entry_set_text(fm.pathentry, fm.path);
    pg_widget_grab_focus(fm.pathentry);
    pg_entry_select_all(fm.pathentry);
}

static bool clear_toast(void *data)
{
    (void)data;
    toast_timer = 0;
    pg_label_set_text(fm.st_busy, "");
    return false;
}

void fm_toast(const char *text)
{
    pg_label_set_text(fm.st_busy, text);
    if (toast_timer)
        pg_timer_remove(toast_timer);
    toast_timer = pg_timer_add(4000, clear_toast, NULL);
}

void fm_status(void)
{
    char left[200], right[160], sz[32];
    int sel = view_count_selected();
    if (fm.loading)
        snprintf(left, sizeof(left), "Loading... %d of %d", fm.load_pos, fm.n);
    else if (sel > 0) {
        uint64_t total = 0;
        for (int i = 0; i < fm.nvis; i++) {
            struct entry *e = &fm.ents[fm.vis[i]];
            if (e->sel && !e->dir)
                total += e->size;
        }
        format_size(total, sz, sizeof(sz));
        snprintf(left, sizeof(left), "%d of %d selected, %s", sel, fm.nvis, sz);
    } else
        snprintf(left, sizeof(left), "%d item%s%s", fm.nvis, fm.nvis == 1 ? "" : "s", fm.filter_text[0] ? " (filtered)" : "");
    pg_label_set_text(fm.st_left, left);
    struct statvfs sv;
    right[0] = 0;
    if (statvfs(fm.path, &sv) == 0 && sv.f_blocks) {
        char fr[32], tot[32];
        unsigned long bs = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
        format_size((uint64_t)sv.f_bavail * bs, fr, sizeof(fr));
        format_size((uint64_t)sv.f_blocks * bs, tot, sizeof(tot));
        snprintf(right, sizeof(right), "%s free of %s", fr, tot);
    }
    pg_label_set_text(fm.st_right, right);
}

static void place_add(const char *label, const char *path, int icon)
{
    if (place_n >= MAX_PLACES)
        return;
    for (int i = 0; i < place_n; i++)
        if (strcmp(place_path[i], path) == 0)
            return;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return;
    place_path[place_n++] = strdup(path);
    pg_list_add(fm.sidebar, icon, label, NULL);
}

static bool skip_fs(const char *type)
{
    static const char *skip[] = {"proc", "procfs", "sysfs", "devfs", "kernelfs", "devtmpfs", "initramfs", "tmpfs", "ramfs", "nodev", "cgroup", "debugfs"};
    for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
        if (strcmp(type, skip[i]) == 0)
            return true;
    return false;
}

void fm_sidebar_refresh(void)
{
    for (int i = 0; i < place_n; i++)
        free(place_path[i]);
    place_n = 0;
    sidebar_lock = true;
    pg_list_clear(fm.sidebar);
    const char *home = getenv("HOME");
    char buf[FM_PATH];
    if (home && *home) {
        place_add("Home", home, PGI_ACCOUNT);
        static const char *sub[] = {"Desktop", "Documents", "Downloads"};
        for (int i = 0; i < 3; i++) {
            join_path(buf, sizeof(buf), home, sub[i]);
            place_add(sub[i], buf, PGI_FOLDER);
        }
    }
    place_add("Root", "/", PGI_HARDDISK);
    place_add("usr", "/usr", PGI_FOLDER);
    place_add("etc", "/etc", PGI_COG);
    place_add("tmp", "/tmp", PGI_TIMER_OUTLINE);
    place_add("var", "/var", PGI_FOLDER);
    FILE *f = fopen("/proc/mounts", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char src[200], mnt[256], type[64];
            if (sscanf(line, "%199s %255s %63s", src, mnt, type) != 3)
                continue;
            if (skip_fs(type) || strcmp(mnt, "/") == 0)
                continue;
            const char *label = strcmp(mnt, "/") == 0 ? "Root" : base_name(mnt);
            place_add(label, mnt, strncmp(src, "/dev", 4) == 0 ? PGI_USB : PGI_HARDDISK);
        }
        fclose(f);
    }
    sidebar_lock = false;
    sync_sidebar();
}

static void on_sidebar(struct pg_widget *w, void *data)
{
    (void)data;
    if (sidebar_lock)
        return;
    int i = pg_list_selected(w);
    if (i >= 0 && i < place_n && strcmp(place_path[i], fm.path) != 0)
        fm_navigate(place_path[i], true);
    pg_widget_grab_focus(fm.view);
}

static void on_back(struct pg_widget *w, void *d) { (void)w; (void)d; fm_go_back(); }
static void on_fwd(struct pg_widget *w, void *d) { (void)w; (void)d; fm_go_forward(); }
static void on_up(struct pg_widget *w, void *d) { (void)w; (void)d; fm_go_up(); }

static void on_view(struct pg_widget *w, void *d)
{
    (void)d;
    int s = pg_segmented_selected(w);
    if (s != fm.grid) {
        fm.grid = s;
        fm.scroll = 0;
        view_ensure_visible();
        view_redraw();
    }
}

static void on_path_activate(struct pg_widget *w, void *d)
{
    (void)d;
    const char *t = pg_entry_text(w);
    if (!t || !*t)
        return;
    char expanded[FM_PATH];
    const char *home = getenv("HOME");
    if (t[0] == '~' && home)
        snprintf(expanded, sizeof(expanded), "%s%s", home, t + 1);
    else if (t[0] != '/')
        join_path(expanded, sizeof(expanded), fm.path, t);
    else
        snprintf(expanded, sizeof(expanded), "%s", t);
    struct stat st;
    if (stat(expanded, &st) == 0 && !S_ISDIR(st.st_mode)) {
        pg_stack_set_index(fm.pathstack, 0);
        pg_widget_grab_focus(fm.view);
        open_path(expanded, false);
        return;
    }
    if (fm_navigate(expanded, true))
        pg_widget_grab_focus(fm.view);
}

static void on_filter(struct pg_widget *w, void *d)
{
    (void)d;
    snprintf(fm.filter_text, sizeof(fm.filter_text), "%s", pg_entry_text(w));
    view_clear_select();
    fm.scroll = 0;
    fm_apply_filter();
}

struct seg {
    int x0, x1;
    char path[FM_PATH];
};

static int crumb_layout(struct pg_widget *w, struct seg *segs, int max)
{
    char parts[64][256];
    int n = 0;
    const char *p = fm.path;
    snprintf(parts[n++], 256, "/");
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        size_t len = strcspn(p, "/");
        if (n < 64) {
            snprintf(parts[n], 256, "%.*s", (int)(len > 255 ? 255 : len), p);
            n++;
        }
        p += len;
    }
    int widths[64];
    int total = 0;
    for (int i = 0; i < n; i++) {
        widths[i] = pgc_text_w(PGF_BODY, i == 0 ? "/" : parts[i]) + 16;
        total += widths[i] + (i ? 10 : 0);
    }
    int avail = w->rect.w - 16;
    int first = 0;
    while (first < n - 1 && total > avail) {
        total -= widths[first] + (first ? 10 : 0);
        first++;
    }
    int x = w->rect.x + 8;
    int count = 0;
    if (first > 0)
        x += 22;
    for (int i = first; i < n && count < max; i++) {
        if (count > 0)
            x += 10;
        segs[count].x0 = x;
        segs[count].x1 = x + widths[i];
        char acc[FM_PATH] = "/";
        size_t off = 1;
        for (int k = 1; k <= i; k++) {
            int r = snprintf(acc + off, sizeof(acc) - off, "%s%s", k > 1 ? "/" : "", parts[k]);
            off += (size_t)(r > 0 ? r : 0);
            if (off >= sizeof(acc))
                off = sizeof(acc) - 1;
        }
        snprintf(segs[count].path, FM_PATH, "%s", acc);
        x += widths[i];
        count++;
    }
    return count;
}

static int crumb_hot = -1;

static void crumb_draw(struct pg_widget *w, struct pg_canvas *c, void *data)
{
    (void)data;
    const struct pg_theme *t = pg_theme();
    pgc_rrect(c, w->rect.x, w->rect.y, w->rect.w, w->rect.h, 8, t->surface2, 255);
    pgc_rrect_stroke(c, w->rect.x, w->rect.y, w->rect.w, w->rect.h, 8, t->border, 255);
    struct seg segs[64];
    int n = crumb_layout(w, segs, 64);
    int fh = pgc_font_h(PGF_BODY);
    int ty = w->rect.y + (w->rect.h - fh) / 2;
    struct pg_clip saved = pgc_clip_push(c, w->rect.x + 2, w->rect.y, w->rect.w - 4, w->rect.h);
    if (n > 0 && strcmp(segs[0].path, "/") != 0)
        pgc_text(c, PGF_BODY, w->rect.x + 10, ty, "...", t->text3);
    for (int i = 0; i < n; i++) {
        bool last = i == n - 1;
        if (i == crumb_hot)
            pgc_rrect(c, segs[i].x0, w->rect.y + 4, segs[i].x1 - segs[i].x0, w->rect.h - 8, 6, t->surface3, 255);
        const char *label = strcmp(segs[i].path, "/") == 0 ? "/" : base_name(segs[i].path);
        pgc_text(c, PGF_BODY, segs[i].x0 + 8, ty, label, last ? t->text : t->text2);
        if (!last)
            pgc_text(c, PGF_BODY, segs[i].x1 + 1, ty, ">", t->text3);
    }
    pgc_clip_pop(c, saved);
}

static bool crumb_input(struct pg_widget *w, const struct pg_event *e, void *data)
{
    (void)data;
    struct seg segs[64];
    int n = crumb_layout(w, segs, 64);
    int hit = -1;
    for (int i = 0; i < n; i++)
        if (e->x >= segs[i].x0 && e->x < segs[i].x1)
            hit = i;
    if (e->type == PG_EVENT_POINTER_MOVE) {
        if (hit != crumb_hot) {
            crumb_hot = hit;
            pg_widget_queue_draw(w);
        }
        return true;
    }
    if (e->type == PG_EVENT_LEAVE) {
        crumb_hot = -1;
        pg_widget_queue_draw(w);
        return false;
    }
    if (e->type == PG_EVENT_POINTER_DOWN && e->button == PG_BUTTON_LEFT) {
        if (hit >= 0) {
            char target[FM_PATH];
            snprintf(target, sizeof(target), "%s", segs[hit].path);
            if (strcmp(target, fm.path) != 0)
                fm_navigate(target, true);
        } else
            fm_focus_path();
        return true;
    }
    return e->type == PG_EVENT_POINTER_UP;
}

static bool on_key(struct pg_window *win, const struct pg_event *e, void *data)
{
    (void)win;
    (void)data;
    if (e->type != PG_EVENT_KEY_DOWN)
        return false;
    struct pg_widget *f = pg_window_focus(fm.win);
    bool ctrl = e->mods & PG_MOD_CTRL, shift = e->mods & PG_MOD_SHIFT, alt = e->mods & PG_MOD_ALT;
    bool typing = f == fm.pathentry || f == fm.filter;
    if (ctrl && e->key == 0x0F) {
        fm_focus_path();
        return true;
    }
    if (ctrl && e->key == 0x09) {
        pg_widget_grab_focus(fm.filter);
        return true;
    }
    if (e->key == 0x3E) {
        fm_reload();
        return true;
    }
    if (typing) {
        if (e->key == PG_KEY_ESCAPE) {
            if (f == fm.filter) {
                pg_entry_set_text(fm.filter, "");
                fm.filter_text[0] = 0;
                fm_apply_filter();
            }
            pg_stack_set_index(fm.pathstack, 0);
            pg_widget_grab_focus(fm.view);
            return true;
        }
        if (f == fm.filter && (e->key == PG_KEY_DOWN || e->key == PG_KEY_ENTER)) {
            pg_widget_grab_focus(fm.view);
            if (fm.cursor < 0 && fm.nvis)
                view_set_cursor(0, false, false);
            return true;
        }
        return false;
    }
    if (menubar_key(e))
        return true;
    if (alt && e->key == PG_KEY_LEFT) {
        fm_go_back();
        return true;
    }
    if (alt && e->key == PG_KEY_RIGHT) {
        fm_go_forward();
        return true;
    }
    if (alt && e->key == PG_KEY_UP) {
        fm_go_up();
        return true;
    }
    if (ctrl && e->key == 0x0B) {
        fm_do(A_HIDDEN);
        return true;
    }
    if (ctrl && shift && e->key == 0x11) {
        ops_new_folder();
        return true;
    }
    if (ctrl && e->key == PG_KEY_W) {
        pg_window_close(fm.win);
        return true;
    }
    if (ctrl && e->key == PG_KEY_Q) {
        pg_quit(0);
        return true;
    }
    if (ctrl && e->key == PG_KEY_C) {
        ops_copy_selection(false);
        return true;
    }
    if (ctrl && e->key == PG_KEY_X) {
        ops_copy_selection(true);
        return true;
    }
    if (ctrl && e->key == PG_KEY_V) {
        ops_paste();
        return true;
    }
    if (ctrl && e->key == PG_KEY_A) {
        view_select_all();
        return true;
    }
    if (ctrl && e->key == 0x12) {
        fm_set_view(!fm.grid);
        return true;
    }
    if (e->key == 0x3B) {
        ops_rename();
        return true;
    }
    if (e->key == PG_KEY_DELETE) {
        ops_delete_selection();
        return true;
    }
    if (e->key == PG_KEY_ENTER && alt) {
        ops_properties();
        return true;
    }
    if (e->key == PG_KEY_BACKSPACE) {
        fm_go_up();
        return true;
    }
    return view_key(e);
}

static bool on_close(struct pg_window *win, void *data)
{
    (void)win;
    (void)data;
    if (ops_busy()) {
        dlg_message("Files", "A file operation is still running. Cancel it first.");
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    const char *run = getenv("FILES_RUN");
    if (run && *run)
        return open_trampoline(run, getenv("FILES_PAUSE") != NULL);
    if (!pg_init(argc, argv)) {
        fprintf(stderr, "files: cannot connect to videod\n");
        return 1;
    }
    fm.sort_asc = true;
    fm.cursor = fm.anchor = -1;
    open_load_config();
    fm.win = pg_window_new("Files", 1000, 640, PG_WINDOW_RESIZABLE);
    if (!fm.win) {
        fprintf(stderr, "files: cannot create a window\n");
        return 1;
    }
    pg_window_set_min_size(fm.win, 640, 400);
    pg_window_on_key(fm.win, on_key, NULL);
    pg_window_on_close(fm.win, on_close, NULL);

    struct pg_widget *root = pg_vbox(0);
    pg_widget_add(root, menubar_new());
    pg_widget_add(root, pg_separator_new());
    struct pg_widget *bar = pg_hbox(6);
    pg_box_set_padding(bar, 10, 8, 10, 8);
    fm.btn_back = pg_button_new("", PGI_CHEVRON_LEFT, PG_BUTTON_FLAT);
    fm.btn_fwd = pg_button_new("", PGI_CHEVRON_RIGHT, PG_BUTTON_FLAT);
    fm.btn_up = pg_button_new("", PGI_UPLOAD, PG_BUTTON_FLAT);
    pg_widget_on(fm.btn_back, PG_SIGNAL_CLICKED, on_back, NULL);
    pg_widget_on(fm.btn_fwd, PG_SIGNAL_CLICKED, on_fwd, NULL);
    pg_widget_on(fm.btn_up, PG_SIGNAL_CLICKED, on_up, NULL);
    pg_widget_add(bar, fm.btn_back);
    pg_widget_add(bar, fm.btn_fwd);
    pg_widget_add(bar, fm.btn_up);
    fm.pathstack = pg_stack_new();
    pg_widget_set_expand(fm.pathstack, true, false);
    fm.crumbs = pg_drawing_new(100, 32, crumb_draw, crumb_input, NULL);
    pg_widget_set_expand(fm.crumbs, true, false);
    fm.pathentry = pg_entry_new("Path");
    pg_widget_set_expand(fm.pathentry, true, false);
    pg_widget_on(fm.pathentry, PG_SIGNAL_ACTIVATE, on_path_activate, NULL);
    pg_widget_add(fm.pathstack, fm.crumbs);
    pg_widget_add(fm.pathstack, fm.pathentry);
    pg_widget_add(bar, fm.pathstack);
    fm.filter = pg_entry_new("Filter");
    pg_widget_set_size(fm.filter, 170, 0);
    pg_widget_on(fm.filter, PG_SIGNAL_CHANGED, on_filter, NULL);
    pg_widget_add(bar, fm.filter);
    static const char *modes[] = {"List", "Grid"};
    fm.btn_view = pg_segmented_new(modes, 2);
    pg_widget_on(fm.btn_view, PG_SIGNAL_CHANGED, on_view, NULL);
    pg_widget_add(bar, fm.btn_view);
    pg_widget_add(bar, hidden_toggle_new());
    pg_widget_add(root, bar);
    pg_widget_add(root, pg_separator_new());

    struct pg_widget *body = pg_hbox(0);
    pg_widget_set_expand(body, true, true);
    fm.sidebar = pg_list_new();
    pg_list_set_sidebar(fm.sidebar, true);
    pg_widget_set_size(fm.sidebar, 190, 0);
    pg_widget_set_expand(fm.sidebar, false, true);
    pg_widget_on(fm.sidebar, PG_SIGNAL_CHANGED, on_sidebar, NULL);
    pg_widget_add(body, fm.sidebar);
    pg_widget_add(body, pg_separator_new());
    fm.view = view_new();
    pg_widget_set_expand(fm.view, true, true);
    pg_widget_add(body, fm.view);
    pg_widget_add(root, body);
    pg_widget_add(root, pg_separator_new());

    struct pg_widget *status = pg_hbox(14);
    pg_box_set_padding(status, 12, 0, 12, 0);
    pg_widget_set_size(status, -1, 24);
    pg_widget_set_expand(status, true, false);
    fm.st_left = pg_label_new_font("", PGF_SMALL, PG_COLOR_TEXT2);
    fm.st_busy = pg_label_new_font("", PGF_SMALL, PG_COLOR_ACCENT);
    fm.st_right = pg_label_new_font("", PGF_SMALL, PG_COLOR_TEXT2);
    pg_widget_add(status, fm.st_left);
    pg_widget_add(status, fm.st_busy);
    struct pg_widget *gap = pg_spacer_new();
    pg_widget_set_expand(gap, true, false);
    pg_widget_add(status, gap);
    pg_widget_add(status, fm.st_right);
    pg_widget_add(root, status);
    pg_window_set_child(fm.win, root);

    fm_sidebar_refresh();
    char start[FM_PATH] = "/";
    const char *home = getenv("HOME");
    if (argc > 1) {
        struct stat st;
        if (stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(start, sizeof(start), "%s", argv[1]);
    } else if (home && *home)
        snprintf(start, sizeof(start), "%s", home);
    if (!fm_navigate(start, true))
        fm_navigate("/", true);
    pg_widget_grab_focus(fm.view);
    int code = pg_run();
    ops_shutdown();
    free_entries();
    for (int i = 0; i < fm.hist_n; i++)
        free(fm.hist[i]);
    free(fm.hist);
    for (int i = 0; i < place_n; i++)
        free(place_path[i]);
    ops_clip_clear();
    pg_shutdown();
    return code;
}
