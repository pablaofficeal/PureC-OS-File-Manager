#include <errno.h>
#include <grp.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fm.h"

struct dlg {
    struct pg_window *win;
    dlg_done_fn fn;
    void *data;
    struct pg_widget *entry;
    bool done;
};

static struct pg_window *dlg_window(const char *title, int w, int h)
{
    struct pg_window *win = pg_window_new(title, w, h, PG_WINDOW_NO_MAXIMIZE | PG_WINDOW_NO_MINIMIZE);
    if (win)
        pg_window_set_background(win, pg_theme()->window);
    return win;
}

static struct pg_widget *dlg_root(void)
{
    struct pg_widget *b = pg_vbox(12);
    pg_box_set_padding(b, 20, 18, 20, 16);
    return b;
}

static void add_lines(struct pg_widget *box, const char *text)
{
    char *copy = strdup(text);
    if (!copy)
        return;
    char *save = copy;
    for (char *line = strsep(&save, "\n"); line; line = strsep(&save, "\n")) {
        struct pg_widget *l = pg_label_new(line);
        pg_label_set_wrap(l, true);
        pg_widget_add(box, l);
    }
    free(copy);
}

static void finish(struct dlg *d, const char *text)
{
    if (d->done)
        return;
    d->done = true;
    if (d->fn)
        d->fn(text, d->data);
    struct pg_window *w = d->win;
    free(d);
    pg_window_destroy(w);
}

static bool dlg_close(struct pg_window *win, void *data)
{
    (void)win;
    finish(data, NULL);
    return false;
}

static bool dlg_key(struct pg_window *win, const struct pg_event *e, void *data)
{
    (void)win;
    if (e->type == PG_EVENT_KEY_DOWN && e->key == PG_KEY_ESCAPE) {
        finish(data, NULL);
        return true;
    }
    return false;
}

static void btn_cancel(struct pg_widget *w, void *data)
{
    (void)w;
    finish(data, NULL);
}

static void btn_ok(struct pg_widget *w, void *data)
{
    (void)w;
    struct dlg *d = data;
    char text[512];
    snprintf(text, sizeof(text), "%s", d->entry ? pg_entry_text(d->entry) : "");
    finish(d, text);
}

static struct dlg *dlg_make(struct pg_window *win, dlg_done_fn fn, void *data)
{
    struct dlg *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->win = win;
    d->fn = fn;
    d->data = data;
    pg_window_on_close(win, dlg_close, d);
    pg_window_on_key(win, dlg_key, d);
    return d;
}

void dlg_message(const char *title, const char *text)
{
    struct pg_window *win = dlg_window(title, 440, 170);
    if (!win)
        return;
    struct dlg *d = dlg_make(win, NULL, NULL);
    if (!d)
        return;
    struct pg_widget *root = dlg_root();
    struct pg_widget *body = pg_vbox(4);
    pg_widget_set_expand(body, true, true);
    add_lines(body, text);
    pg_widget_add(root, body);
    struct pg_widget *row = pg_hbox(8);
    pg_widget_add(row, pg_spacer_new());
    struct pg_widget *ok = pg_button_new("OK", PG_NO_ICON, PG_BUTTON_PRIMARY);
    pg_widget_on(ok, PG_SIGNAL_CLICKED, btn_cancel, d);
    pg_widget_add(row, ok);
    pg_widget_add(root, row);
    pg_window_set_child(win, root);
    pg_window_set_focus(win, ok);
}

void dlg_confirm(const char *title, const char *text, const char *ok, bool danger, dlg_done_fn fn, void *data)
{
    struct pg_window *win = dlg_window(title, 440, 170);
    struct dlg *d = win ? dlg_make(win, fn, data) : NULL;
    if (!d) {
        if (win)
            pg_window_destroy(win);
        if (fn)
            fn(NULL, data);
        return;
    }
    struct pg_widget *root = dlg_root();
    struct pg_widget *body = pg_vbox(4);
    pg_widget_set_expand(body, true, true);
    add_lines(body, text);
    pg_widget_add(root, body);
    struct pg_widget *row = pg_hbox(8);
    pg_widget_add(row, pg_spacer_new());
    struct pg_widget *cancel = pg_button_new("Cancel", PG_NO_ICON, PG_BUTTON_SECONDARY);
    struct pg_widget *yes = pg_button_new(ok, PG_NO_ICON, danger ? PG_BUTTON_DANGER : PG_BUTTON_PRIMARY);
    pg_widget_on(cancel, PG_SIGNAL_CLICKED, btn_cancel, d);
    pg_widget_on(yes, PG_SIGNAL_CLICKED, btn_ok, d);
    pg_widget_add(row, cancel);
    pg_widget_add(row, yes);
    pg_widget_add(root, row);
    pg_window_set_child(win, root);
    pg_window_set_focus(win, cancel);
}

static void entry_activate(struct pg_widget *w, void *data)
{
    (void)w;
    btn_ok(NULL, data);
}

void dlg_input(const char *title, const char *label, const char *initial, const char *ok, dlg_done_fn fn, void *data)
{
    struct pg_window *win = dlg_window(title, 440, 170);
    struct dlg *d = win ? dlg_make(win, fn, data) : NULL;
    if (!d) {
        if (win)
            pg_window_destroy(win);
        if (fn)
            fn(NULL, data);
        return;
    }
    struct pg_widget *root = dlg_root();
    pg_widget_add(root, pg_label_new(label));
    d->entry = pg_entry_new("");
    pg_entry_set_max_length(d->entry, 250);
    pg_entry_set_text(d->entry, initial ? initial : "");
    pg_widget_on(d->entry, PG_SIGNAL_ACTIVATE, entry_activate, d);
    pg_widget_add(root, d->entry);
    pg_widget_add(root, pg_spacer_new());
    struct pg_widget *row = pg_hbox(8);
    pg_widget_add(row, pg_spacer_new());
    struct pg_widget *cancel = pg_button_new("Cancel", PG_NO_ICON, PG_BUTTON_SECONDARY);
    struct pg_widget *yes = pg_button_new(ok, PG_NO_ICON, PG_BUTTON_PRIMARY);
    pg_widget_on(cancel, PG_SIGNAL_CLICKED, btn_cancel, d);
    pg_widget_on(yes, PG_SIGNAL_CLICKED, btn_ok, d);
    pg_widget_add(row, cancel);
    pg_widget_add(row, yes);
    pg_widget_add(root, row);
    pg_window_set_child(win, root);
    pg_window_set_focus(win, d->entry);
    pg_entry_select_all(d->entry);
}

static void open_with_done(const char *text, void *data)
{
    char *path = data;
    if (text && *text)
        open_with_command(text, path);
    free(path);
}

void dlg_open_with(const char *path)
{
    char *copy = strdup(path);
    if (!copy)
        return;
    dlg_input("Open with", "Command (%f is replaced by the file)", "", "Open", open_with_done, copy);
}

struct cflt {
    struct pg_window *win;
    struct pg_widget *all;
    void (*answer)(int, bool);
    bool done;
};

static void cflt_pick(struct cflt *c, int choice)
{
    if (c->done)
        return;
    c->done = true;
    bool all = pg_checkbox_active(c->all);
    void (*answer)(int, bool) = c->answer;
    struct pg_window *w = c->win;
    free(c);
    answer(choice, all);
    pg_window_destroy(w);
}

static void cflt_skip(struct pg_widget *w, void *d) { (void)w; cflt_pick(d, CF_SKIP); }
static void cflt_keep(struct pg_widget *w, void *d) { (void)w; cflt_pick(d, CF_KEEP); }
static void cflt_replace(struct pg_widget *w, void *d) { (void)w; cflt_pick(d, CF_REPLACE); }
static void cflt_cancel(struct pg_widget *w, void *d) { (void)w; cflt_pick(d, CF_CANCEL); }

static bool cflt_close(struct pg_window *win, void *data)
{
    (void)win;
    cflt_pick(data, CF_CANCEL);
    return false;
}

void dlg_conflict(const char *name, bool all_possible, void (*answer)(int choice, bool all))
{
    struct pg_window *win = dlg_window("File already exists", 520, 190);
    struct cflt *c = win ? calloc(1, sizeof(*c)) : NULL;
    if (!c) {
        if (win)
            pg_window_destroy(win);
        answer(CF_CANCEL, false);
        return;
    }
    c->win = win;
    c->answer = answer;
    pg_window_on_close(win, cflt_close, c);
    struct pg_widget *root = dlg_root();
    char msg[400];
    snprintf(msg, sizeof(msg), "\"%s\" already exists in the destination.", name);
    struct pg_widget *l = pg_label_new(msg);
    pg_label_set_wrap(l, true);
    pg_widget_add(root, l);
    c->all = pg_checkbox_new("Apply to all remaining conflicts", false);
    pg_widget_set_enabled(c->all, all_possible);
    pg_widget_add(root, c->all);
    pg_widget_add(root, pg_spacer_new());
    struct pg_widget *row = pg_hbox(8);
    struct pg_widget *cancel = pg_button_new("Cancel", PG_NO_ICON, PG_BUTTON_GHOST);
    struct pg_widget *skip = pg_button_new("Skip", PG_NO_ICON, PG_BUTTON_SECONDARY);
    struct pg_widget *keep = pg_button_new("Keep both", PG_NO_ICON, PG_BUTTON_PRIMARY);
    struct pg_widget *rep = pg_button_new("Replace", PG_NO_ICON, PG_BUTTON_DANGER);
    pg_widget_on(cancel, PG_SIGNAL_CLICKED, cflt_cancel, c);
    pg_widget_on(skip, PG_SIGNAL_CLICKED, cflt_skip, c);
    pg_widget_on(keep, PG_SIGNAL_CLICKED, cflt_keep, c);
    pg_widget_on(rep, PG_SIGNAL_CLICKED, cflt_replace, c);
    pg_widget_add(row, cancel);
    pg_widget_add(row, pg_spacer_new());
    pg_widget_add(row, skip);
    pg_widget_add(row, keep);
    pg_widget_add(row, rep);
    pg_widget_add(root, row);
    pg_window_set_child(win, root);
}

static struct pg_widget *prog_label, *prog_bar;
static void (*prog_cancel)(void);

static void prog_btn(struct pg_widget *w, void *d)
{
    (void)w;
    (void)d;
    if (prog_cancel)
        prog_cancel();
}

static bool prog_close(struct pg_window *win, void *d)
{
    (void)win;
    (void)d;
    if (prog_cancel)
        prog_cancel();
    return false;
}

struct pg_window *dlg_progress(const char *title, void (*cancel)(void))
{
    struct pg_window *win = dlg_window(title, 440, 160);
    if (!win)
        return NULL;
    prog_cancel = cancel;
    pg_window_on_close(win, prog_close, NULL);
    struct pg_widget *root = dlg_root();
    prog_label = pg_label_new("Preparing...");
    pg_label_set_ellipsize(prog_label, true);
    prog_bar = pg_progress_new();
    pg_widget_add(root, prog_label);
    pg_widget_add(root, prog_bar);
    pg_widget_add(root, pg_spacer_new());
    struct pg_widget *row = pg_hbox(8);
    pg_widget_add(row, pg_spacer_new());
    struct pg_widget *b = pg_button_new("Cancel", PG_NO_ICON, PG_BUTTON_SECONDARY);
    pg_widget_on(b, PG_SIGNAL_CLICKED, prog_btn, NULL);
    pg_widget_add(row, b);
    pg_widget_add(root, row);
    pg_window_set_child(win, root);
    return win;
}

void dlg_progress_update(struct pg_window *w, const char *line, uint32_t permille)
{
    if (!w || !prog_label)
        return;
    pg_label_set_text(prog_label, line[0] ? line : "Working...");
    pg_progress_set(prog_bar, permille);
}

struct props {
    struct pg_window *win;
    char path[FM_PATH];
    bool dir;
    mode_t mode;
    pthread_t th;
    bool threaded;
    volatile bool cancel;
    volatile bool done;
    uint64_t bytes, files, dirs;
    struct pg_widget *size_kv, *contents_kv;
    struct pg_widget *perm[9];
    struct pg_widget *mode_kv;
    uint32_t timer;
    bool closed;
};

static void *props_worker(void *arg)
{
    struct props *p = arg;
    ops_dir_size(p->path, &p->bytes, &p->files, &p->dirs, &p->cancel);
    p->done = true;
    return NULL;
}

static void props_show(struct props *p)
{
    char a[48], b[96], line[160];
    format_size(p->bytes, a, sizeof(a));
    snprintf(b, sizeof(b), "%s (%llu bytes)", a, (unsigned long long)p->bytes);
    pg_kv_set_value(p->size_kv, b);
    if (p->contents_kv) {
        snprintf(line, sizeof(line), "%llu files, %llu folders%s", (unsigned long long)p->files,
                 (unsigned long long)(p->dirs ? p->dirs - 1 : 0), p->done ? "" : " (counting...)");
        pg_kv_set_value(p->contents_kv, line);
    }
}

static bool props_tick(void *data)
{
    struct props *p = data;
    props_show(p);
    if (p->done) {
        p->timer = 0;
        return false;
    }
    return true;
}

static void props_close(struct props *p)
{
    if (p->closed)
        return;
    p->closed = true;
    p->cancel = true;
    if (p->timer)
        pg_timer_remove(p->timer);
    if (p->threaded)
        pthread_join(p->th, NULL);
    struct pg_window *w = p->win;
    free(p);
    pg_window_destroy(w);
}

static bool props_on_close(struct pg_window *win, void *data)
{
    (void)win;
    props_close(data);
    return false;
}

static bool props_on_key(struct pg_window *win, const struct pg_event *e, void *data)
{
    (void)win;
    if (e->type == PG_EVENT_KEY_DOWN && e->key == PG_KEY_ESCAPE) {
        props_close(data);
        return true;
    }
    return false;
}

static void props_ok(struct pg_widget *w, void *data)
{
    (void)w;
    props_close(data);
}

static void mode_string(mode_t m, char *out)
{
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        out[i] = (m & (0400 >> i)) ? rwx[i] : '-';
    out[9] = 0;
}

static void props_apply(struct pg_widget *w, void *data)
{
    (void)w;
    struct props *p = data;
    mode_t m = p->mode & ~0777u;
    for (int i = 0; i < 9; i++)
        if (pg_checkbox_active(p->perm[i]))
            m |= (mode_t)(0400 >> i);
    if (chmod(p->path, m & 07777) != 0) {
        char msg[400];
        snprintf(msg, sizeof(msg), "%s: %s", base_name(p->path), ops_strerror(errno));
        dlg_message("Cannot change permissions", msg);
        return;
    }
    p->mode = m | (p->mode & S_IFMT);
    char s[16];
    mode_string(m, s);
    char line[48];
    snprintf(line, sizeof(line), "%s (%04o)", s, (unsigned)(m & 07777));
    pg_kv_set_value(p->mode_kv, line);
    fm_reload();
}

static void kv(struct pg_widget *box, const char *k, const char *v)
{
    pg_widget_add(box, pg_kv_new(k, v));
}

void dlg_properties(const char *path)
{
    struct stat st, ls;
    if (lstat(path, &ls) != 0) {
        char msg[FM_PATH + 100];
        snprintf(msg, sizeof(msg), "%s: %s", path, ops_strerror(errno));
        dlg_message("Properties", msg);
        return;
    }
    st = ls;
    if (S_ISLNK(ls.st_mode) && stat(path, &st) != 0)
        st = ls;
    struct pg_window *win = dlg_window("Properties", 480, 600);
    struct props *p = win ? calloc(1, sizeof(*p)) : NULL;
    if (!p) {
        if (win)
            pg_window_destroy(win);
        return;
    }
    p->win = win;
    snprintf(p->path, sizeof(p->path), "%s", path);
    p->dir = S_ISDIR(st.st_mode);
    p->mode = st.st_mode;
    pg_window_on_close(win, props_on_close, p);
    pg_window_on_key(win, props_on_key, p);

    struct pg_widget *root = dlg_root();
    pg_box_set_spacing(root, 6);
    char buf[FM_PATH + 64], dir[FM_PATH];
    parent_path(path, dir, sizeof(dir));
    struct pg_widget *head = pg_label_new_font(strcmp(path, "/") == 0 ? "/" : base_name(path), PGF_HEAD, PG_COLOR_TEXT);
    pg_label_set_ellipsize(head, true);
    pg_widget_add(root, head);
    pg_widget_add(root, pg_separator_new());
    kv(root, "Location", dir);
    struct entry tmp = {0};
    tmp.name = (char *)base_name(path);
    tmp.dir = p->dir;
    tmp.link = S_ISLNK(ls.st_mode);
    tmp.mode = st.st_mode;
    tmp.stated = true;
    kv(root, "Type", type_name_for(&tmp));
    if (S_ISLNK(ls.st_mode)) {
        char target[FM_PATH];
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        target[n > 0 ? n : 0] = 0;
        kv(root, "Link target", target);
    }
    p->bytes = p->dir ? 0 : (uint64_t)st.st_size;
    p->size_kv = pg_kv_new("Size", "");
    pg_widget_add(root, p->size_kv);
    if (p->dir) {
        p->contents_kv = pg_kv_new("Contains", "");
        pg_widget_add(root, p->contents_kv);
        p->threaded = pthread_create(&p->th, NULL, props_worker, p) == 0;
        if (!p->threaded)
            p->done = true;
        else
            p->timer = pg_timer_add(150, props_tick, p);
    } else
        p->done = true;
    props_show(p);
    format_time((int64_t)st.st_mtime, buf, sizeof(buf));
    kv(root, "Modified", buf);
    format_time((int64_t)st.st_atime, buf, sizeof(buf));
    kv(root, "Accessed", buf);
    format_time((int64_t)st.st_ctime, buf, sizeof(buf));
    kv(root, "Changed", buf);
    struct passwd *pw = getpwuid(st.st_uid);
    struct group *gr = getgrgid(st.st_gid);
    snprintf(buf, sizeof(buf), "%s (%u)", pw ? pw->pw_name : "?", (unsigned)st.st_uid);
    kv(root, "Owner", buf);
    snprintf(buf, sizeof(buf), "%s (%u)", gr ? gr->gr_name : "?", (unsigned)st.st_gid);
    kv(root, "Group", buf);
    char ms[16];
    mode_string(st.st_mode, ms);
    snprintf(buf, sizeof(buf), "%s (%04o)", ms, (unsigned)(st.st_mode & 07777));
    p->mode_kv = pg_kv_new("Permissions", buf);
    pg_widget_add(root, p->mode_kv);

    struct pg_widget *grid = pg_grid_new(18, 4);
    static const char *rows[] = {"Owner", "Group", "Others"};
    static const char *colsn[] = {"Read", "Write", "Execute"};
    for (int c = 0; c < 3; c++)
        pg_grid_attach(grid, pg_label_new_font(colsn[c], PGF_SMALL, PG_COLOR_TEXT2), c + 1, 0, 1, 1);
    for (int r = 0; r < 3; r++) {
        pg_grid_attach(grid, pg_label_new(rows[r]), 0, r + 1, 1, 1);
        for (int c = 0; c < 3; c++) {
            int bit = r * 3 + c;
            p->perm[bit] = pg_checkbox_new("", (st.st_mode & (0400 >> bit)) != 0);
            pg_grid_attach(grid, p->perm[bit], c + 1, r + 1, 1, 1);
        }
    }
    pg_widget_add(root, grid);
    pg_widget_add(root, pg_spacer_new());
    struct pg_widget *row = pg_hbox(8);
    struct pg_widget *apply = pg_button_new("Apply permissions", PG_NO_ICON, PG_BUTTON_SECONDARY);
    pg_widget_on(apply, PG_SIGNAL_CLICKED, props_apply, p);
    pg_widget_set_enabled(apply, !S_ISLNK(ls.st_mode));
    struct pg_widget *close = pg_button_new("Close", PG_NO_ICON, PG_BUTTON_PRIMARY);
    pg_widget_on(close, PG_SIGNAL_CLICKED, props_ok, p);
    pg_widget_add(row, apply);
    pg_widget_add(row, pg_spacer_new());
    pg_widget_add(row, close);
    pg_widget_add(root, row);
    pg_window_set_child(win, root);
}
