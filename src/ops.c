#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fm.h"

#define BUF_SIZE 65536
#define ITEM_COST 4096

enum job_kind { J_COPY, J_MOVE, J_DELETE };

struct job {
    pthread_t th;
    bool active;
    bool started;
    volatile bool done;
    volatile bool cancel;
    int kind;
    char **src;
    int n;
    char dest[FM_PATH];
    volatile uint64_t total;
    volatile uint64_t progress;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    char cur[256];
    int ask;
    bool ask_shown;
    char ask_name[256];
    int answer;
    int policy;
    int errors;
    char first_err[400];
    struct pg_window *dlg;
    uint32_t timer;
    char sel_name[256];
};

static struct job job;

const char *ops_strerror(int err)
{
    return strerror(err);
}

bool ops_busy(void)
{
    return job.active;
}

void ops_clip_clear(void)
{
    for (int i = 0; i < fm.clip_n; i++)
        free(fm.clip[i]);
    free(fm.clip);
    fm.clip = NULL;
    fm.clip_n = 0;
}

void ops_copy_selection(bool cut)
{
    int n;
    char **p = view_selection_paths(&n);
    if (n == 0) {
        free(p);
        return;
    }
    ops_clip_clear();
    fm.clip = p;
    fm.clip_n = n;
    fm.clip_cut = cut;
    char msg[64];
    snprintf(msg, sizeof(msg), "%d item%s %s", n, n == 1 ? "" : "s", cut ? "cut" : "copied");
    fm_toast(msg);
}

static void note_error(const char *path, int err)
{
    pthread_mutex_lock(&job.mu);
    if (job.errors++ == 0)
        snprintf(job.first_err, sizeof(job.first_err), "%s: %s", path, strerror(err));
    pthread_mutex_unlock(&job.mu);
}

static void set_current(const char *path)
{
    pthread_mutex_lock(&job.mu);
    snprintf(job.cur, sizeof(job.cur), "%s", base_name(path));
    pthread_mutex_unlock(&job.mu);
}

static uint64_t count_tree(const char *path)
{
    struct stat st;
    if (job.cancel || lstat(path, &st) != 0)
        return 0;
    uint64_t total = ITEM_COST;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d)
            return total;
        struct dirent *de;
        while ((de = readdir(d)) && !job.cancel) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char sub[FM_PATH];
            join_path(sub, sizeof(sub), path, de->d_name);
            total += count_tree(sub);
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode))
        total += (uint64_t)st.st_size;
    return total;
}

void ops_make_unique(const char *dir, const char *name, char *out, size_t cap)
{
    char full[FM_PATH];
    struct stat st;
    join_path(full, sizeof(full), dir, name);
    if (lstat(full, &st) != 0) {
        snprintf(out, cap, "%s", name);
        return;
    }
    const char *ext = ext_of(name);
    size_t stem = *ext ? (size_t)(ext - name - 1) : strlen(name);
    if (strlen(name) > 11 && strcmp(name + strlen(name) - 11, ".pkg.tar.gz") == 0) {
        stem = strlen(name) - 11;
        ext = "pkg.tar.gz";
    }
    for (int i = 2; i < 10000; i++) {
        if (*ext)
            snprintf(out, cap, "%.*s (%d).%s", (int)stem, name, i, ext);
        else
            snprintf(out, cap, "%.*s (%d)", (int)stem, name, i);
        join_path(full, sizeof(full), dir, out);
        if (lstat(full, &st) != 0)
            return;
    }
    snprintf(out, cap, "%s", name);
}

static int remove_tree(const char *path)
{
    struct stat st;
    if (job.cancel)
        return -1;
    if (lstat(path, &st) != 0)
        return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) && !job.cancel) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                    continue;
                char sub[FM_PATH];
                join_path(sub, sizeof(sub), path, de->d_name);
                remove_tree(sub);
            }
            closedir(d);
        } else
            note_error(path, errno);
        if (job.cancel)
            return -1;
        if (rmdir(path) != 0) {
            note_error(path, errno);
            return -1;
        }
    } else if (unlink(path) != 0) {
        note_error(path, errno);
        return -1;
    }
    set_current(path);
    job.progress += ITEM_COST;
    return 0;
}

static int ask_conflict(const char *name)
{
    pthread_mutex_lock(&job.mu);
    if (job.policy) {
        int p = job.policy;
        pthread_mutex_unlock(&job.mu);
        return p;
    }
    snprintf(job.ask_name, sizeof(job.ask_name), "%s", name);
    job.ask_shown = false;
    job.ask = 1;
    while (job.ask && !job.cancel)
        pthread_cond_wait(&job.cv, &job.mu);
    int a = job.cancel ? CF_CANCEL : job.answer;
    job.ask = 0;
    pthread_mutex_unlock(&job.mu);
    if (a == CF_CANCEL)
        job.cancel = true;
    return a;
}

static bool is_inside(const char *dir, const char *parent)
{
    size_t n = strlen(parent);
    return strncmp(dir, parent, n) == 0 && (dir[n] == '/' || dir[n] == 0);
}

static int copy_file(const char *src, const char *dst, const struct stat *st)
{
    int in = open(src, O_RDONLY);
    if (in < 0) {
        note_error(src, errno);
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st->st_mode & 07777);
    if (out < 0) {
        note_error(dst, errno);
        close(in);
        return -1;
    }
    char *buf = malloc(BUF_SIZE);
    int rc = 0;
    if (!buf) {
        note_error(src, ENOMEM);
        rc = -1;
    }
    while (rc == 0) {
        if (job.cancel) {
            rc = -1;
            break;
        }
        ssize_t r = read(in, buf, BUF_SIZE);
        if (r < 0) {
            note_error(src, errno);
            rc = -1;
            break;
        }
        if (r == 0)
            break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w <= 0) {
                note_error(dst, w < 0 ? errno : EIO);
                rc = -1;
                break;
            }
            off += w;
        }
        job.progress += (uint64_t)r;
    }
    free(buf);
    close(in);
    close(out);
    if (rc != 0)
        unlink(dst);
    else
        chmod(dst, st->st_mode & 07777);
    job.progress += ITEM_COST;
    return rc;
}

static int transfer(const char *src, const char *dst_in, bool move, bool top);

static int copy_tree(const char *src, const char *dst, bool move)
{
    struct stat st;
    if (lstat(src, &st) != 0) {
        note_error(src, errno);
        return -1;
    }
    set_current(src);
    if (S_ISDIR(st.st_mode)) {
        struct stat ds;
        if (lstat(dst, &ds) != 0 && mkdir(dst, (st.st_mode & 07777) | 0700) != 0) {
            note_error(dst, errno);
            return -1;
        }
        DIR *d = opendir(src);
        if (!d) {
            note_error(src, errno);
            return -1;
        }
        int rc = 0;
        struct dirent *de;
        while ((de = readdir(d)) && !job.cancel) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char s2[FM_PATH], d2[FM_PATH];
            join_path(s2, sizeof(s2), src, de->d_name);
            join_path(d2, sizeof(d2), dst, de->d_name);
            if (transfer(s2, d2, move, false) != 0)
                rc = -1;
        }
        closedir(d);
        job.progress += ITEM_COST;
        if (rc == 0 && !job.cancel)
            chmod(dst, st.st_mode & 07777);
        return job.cancel ? -1 : rc;
    }
    if (S_ISLNK(st.st_mode)) {
        char target[FM_PATH];
        ssize_t n = readlink(src, target, sizeof(target) - 1);
        if (n < 0) {
            note_error(src, errno);
            return -1;
        }
        target[n] = 0;
        if (symlink(target, dst) != 0) {
            note_error(dst, errno);
            return -1;
        }
        job.progress += ITEM_COST;
        return 0;
    }
    if (S_ISREG(st.st_mode))
        return copy_file(src, dst, &st);
    note_error(src, ENOTSUP);
    return -1;
}

static int transfer(const char *src, const char *dst_in, bool move, bool top)
{
    char dst[FM_PATH];
    snprintf(dst, sizeof(dst), "%s", dst_in);
    struct stat ss, ds;
    if (job.cancel)
        return -1;
    if (lstat(src, &ss) != 0) {
        note_error(src, errno);
        return -1;
    }
    if (strcmp(src, dst) == 0) {
        if (move)
            return 0;
        char dir[FM_PATH], uniq[256];
        parent_path(dst, dir, sizeof(dir));
        ops_make_unique(dir, base_name(dst), uniq, sizeof(uniq));
        join_path(dst, sizeof(dst), dir, uniq);
    } else if (S_ISDIR(ss.st_mode) && !S_ISLNK(ss.st_mode) && is_inside(dst, src)) {
        note_error(src, EINVAL);
        return -1;
    }
    bool merge = false;
    if (lstat(dst, &ds) == 0) {
        if (S_ISDIR(ss.st_mode) && S_ISDIR(ds.st_mode))
            merge = true;
        else {
            int a = ask_conflict(base_name(dst));
            if (a == CF_CANCEL)
                return -1;
            if (a == CF_SKIP) {
                job.progress += count_tree(src);
                return 0;
            }
            if (a == CF_KEEP) {
                char dir[FM_PATH], uniq[256];
                parent_path(dst, dir, sizeof(dir));
                ops_make_unique(dir, base_name(dst), uniq, sizeof(uniq));
                join_path(dst, sizeof(dst), dir, uniq);
            } else if (a == CF_REPLACE) {
                if (S_ISDIR(ds.st_mode))
                    remove_tree(dst);
                else if (unlink(dst) != 0) {
                    note_error(dst, errno);
                    return -1;
                }
            }
        }
    }
    (void)top;
    if (move && !merge && rename(src, dst) == 0) {
        job.progress += count_tree(dst);
        set_current(src);
        return 0;
    }
    int rc = copy_tree(src, dst, move);
    if (rc == 0 && move && !job.cancel) {
        if (S_ISDIR(ss.st_mode)) {
            if (rmdir(src) != 0)
                note_error(src, errno);
        } else if (unlink(src) != 0) {
            note_error(src, errno);
            rc = -1;
        }
    }
    return rc;
}

static void *worker(void *arg)
{
    struct job *j = arg;
    uint64_t total = 0;
    for (int i = 0; i < j->n && !j->cancel; i++)
        total += count_tree(j->src[i]);
    j->total = total ? total : 1;
    for (int i = 0; i < j->n && !j->cancel; i++) {
        if (j->kind == J_DELETE) {
            remove_tree(j->src[i]);
            continue;
        }
        char dst[FM_PATH];
        join_path(dst, sizeof(dst), j->dest, base_name(j->src[i]));
        if (transfer(j->src[i], dst, j->kind == J_MOVE, true) == 0 && i == 0)
            snprintf(j->sel_name, sizeof(j->sel_name), "%s", base_name(dst));
    }
    j->done = true;
    return NULL;
}

static void job_cancel(void)
{
    pthread_mutex_lock(&job.mu);
    job.cancel = true;
    pthread_cond_signal(&job.cv);
    pthread_mutex_unlock(&job.mu);
}

static void conflict_answer(int choice, bool all)
{
    pthread_mutex_lock(&job.mu);
    job.answer = choice;
    if (all && choice != CF_CANCEL)
        job.policy = choice;
    job.ask = 0;
    pthread_cond_signal(&job.cv);
    pthread_mutex_unlock(&job.mu);
}

static void job_finish(void)
{
    pthread_join(job.th, NULL);
    if (job.dlg)
        pg_window_destroy(job.dlg);
    job.dlg = NULL;
    for (int i = 0; i < job.n; i++)
        free(job.src[i]);
    free(job.src);
    int errors = job.errors;
    char err[400];
    snprintf(err, sizeof(err), "%s", job.first_err);
    char sel[256];
    snprintf(sel, sizeof(sel), "%s", job.sel_name);
    bool canceled = job.cancel;
    job.active = false;
    job.timer = 0;
    pthread_mutex_destroy(&job.mu);
    pthread_cond_destroy(&job.cv);
    if (sel[0])
        snprintf(fm.pending_select, sizeof(fm.pending_select), "%s", sel);
    fm_reload();
    if (errors) {
        char msg[560];
        snprintf(msg, sizeof(msg), "%d error%s occurred.\n%s", errors, errors == 1 ? "" : "s", err);
        dlg_message("Operation finished with errors", msg);
    } else
        fm_toast(canceled ? "Cancelled" : "Done");
}

static bool job_poll(void *data)
{
    (void)data;
    if (!job.active)
        return false;
    char line[320];
    pthread_mutex_lock(&job.mu);
    snprintf(line, sizeof(line), "%s", job.cur);
    bool ask = job.ask && !job.ask_shown;
    char name[256];
    snprintf(name, sizeof(name), "%s", job.ask_name);
    if (ask)
        job.ask_shown = true;
    pthread_mutex_unlock(&job.mu);
    if (job.done) {
        job_finish();
        return false;
    }
    uint64_t total = job.total, prog = job.progress;
    uint32_t pm = total ? (uint32_t)(prog * 1000 / total) : 0;
    if (pm > 1000)
        pm = 1000;
    if (job.dlg)
        dlg_progress_update(job.dlg, line, pm);
    if (ask)
        dlg_conflict(name, job.n > 1 || true, conflict_answer);
    return true;
}

static void job_run(int kind, char **srcs, int n, const char *dest)
{
    memset(&job, 0, sizeof(job));
    pthread_mutex_init(&job.mu, NULL);
    pthread_cond_init(&job.cv, NULL);
    job.kind = kind;
    job.src = srcs;
    job.n = n;
    snprintf(job.dest, sizeof(job.dest), "%s", dest ? dest : "");
    job.active = true;
    const char *title = kind == J_COPY ? "Copying" : kind == J_MOVE ? "Moving" : "Deleting";
    job.dlg = dlg_progress(title, job_cancel);
    if (pthread_create(&job.th, NULL, worker, &job) != 0) {
        job.done = true;
        job.errors = 1;
        snprintf(job.first_err, sizeof(job.first_err), "cannot start a worker thread");
        job.th = 0;
        job.active = false;
        for (int i = 0; i < n; i++)
            free(srcs[i]);
        free(srcs);
        if (job.dlg)
            pg_window_destroy(job.dlg);
        job.dlg = NULL;
        dlg_message("Files", "Cannot start a worker thread.");
        return;
    }
    job.started = true;
    job.timer = pg_timer_add(80, job_poll, NULL);
}

void ops_start_transfer(char **srcs, int n, const char *dest, bool move)
{
    if (job.active) {
        for (int i = 0; i < n; i++)
            free(srcs[i]);
        free(srcs);
        fm_toast("Another operation is running");
        return;
    }
    job_run(move ? J_MOVE : J_COPY, srcs, n, dest);
}

void ops_start_delete(char **srcs, int n)
{
    if (job.active) {
        for (int i = 0; i < n; i++)
            free(srcs[i]);
        free(srcs);
        fm_toast("Another operation is running");
        return;
    }
    job_run(J_DELETE, srcs, n, NULL);
}

void ops_shutdown(void)
{
    if (!job.active)
        return;
    job_cancel();
    pthread_join(job.th, NULL);
    job.active = false;
}

void ops_paste(void)
{
    if (!fm.clip_n)
        return;
    bool cut = fm.clip_cut;
    char **copy = calloc((size_t)fm.clip_n, sizeof(char *));
    if (!copy)
        return;
    int n = 0;
    for (int i = 0; i < fm.clip_n; i++) {
        char parent[FM_PATH];
        parent_path(fm.clip[i], parent, sizeof(parent));
        if (cut && strcmp(parent, fm.path) == 0)
            continue;
        copy[n++] = strdup(fm.clip[i]);
    }
    if (n == 0) {
        free(copy);
        if (cut)
            ops_clip_clear();
        return;
    }
    if (cut)
        ops_clip_clear();
    ops_start_transfer(copy, n, fm.path, cut);
}

struct del_ctx {
    char **paths;
    int n;
};

static void delete_done(const char *text, void *data)
{
    struct del_ctx *c = data;
    if (text)
        ops_start_delete(c->paths, c->n);
    else
        view_free_paths(c->paths, c->n);
    free(c);
}

void ops_delete_selection(void)
{
    int n;
    char **p = view_selection_paths(&n);
    if (n == 0) {
        free(p);
        return;
    }
    struct del_ctx *c = malloc(sizeof(*c));
    if (!c) {
        view_free_paths(p, n);
        return;
    }
    c->paths = p;
    c->n = n;
    char msg[400];
    if (n == 1)
        snprintf(msg, sizeof(msg), "Delete \"%s\" permanently? This cannot be undone.", base_name(p[0]));
    else
        snprintf(msg, sizeof(msg), "Delete %d items permanently? This cannot be undone.", n);
    dlg_confirm("Delete", msg, "Delete", true, delete_done, c);
}

static bool valid_name(const char *t)
{
    return t && *t && !strchr(t, '/') && strcmp(t, ".") != 0 && strcmp(t, "..") != 0;
}

static void mkdir_done(const char *text, void *data)
{
    (void)data;
    if (!text)
        return;
    if (!valid_name(text)) {
        dlg_message("New folder", "Invalid name.");
        return;
    }
    char full[FM_PATH];
    join_path(full, sizeof(full), fm.path, text);
    if (mkdir(full, 0755) != 0) {
        char msg[400];
        snprintf(msg, sizeof(msg), "%s: %s", text, strerror(errno));
        dlg_message("Cannot create folder", msg);
        return;
    }
    snprintf(fm.pending_select, sizeof(fm.pending_select), "%s", text);
    fm_reload();
}

static void mkfile_done(const char *text, void *data)
{
    (void)data;
    if (!text)
        return;
    if (!valid_name(text)) {
        dlg_message("New file", "Invalid name.");
        return;
    }
    char full[FM_PATH];
    join_path(full, sizeof(full), fm.path, text);
    int fd = open(full, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        char msg[400];
        snprintf(msg, sizeof(msg), "%s: %s", text, strerror(errno));
        dlg_message("Cannot create file", msg);
        return;
    }
    close(fd);
    snprintf(fm.pending_select, sizeof(fm.pending_select), "%s", text);
    fm_reload();
}

void ops_new_folder(void)
{
    char name[256];
    ops_make_unique(fm.path, "New folder", name, sizeof(name));
    dlg_input("New folder", "Folder name", name, "Create", mkdir_done, NULL);
}

void ops_new_file(void)
{
    char name[256];
    ops_make_unique(fm.path, "New file", name, sizeof(name));
    dlg_input("New file", "File name", name, "Create", mkfile_done, NULL);
}

struct ren_ctx {
    char old[FM_PATH];
};

static void rename_done(const char *text, void *data)
{
    struct ren_ctx *c = data;
    if (text && strcmp(text, base_name(c->old)) != 0) {
        if (!valid_name(text))
            dlg_message("Rename", "Invalid name.");
        else {
            char dir[FM_PATH], full[FM_PATH];
            parent_path(c->old, dir, sizeof(dir));
            join_path(full, sizeof(full), dir, text);
            struct stat st;
            if (lstat(full, &st) == 0)
                dlg_message("Rename", "An item with this name already exists.");
            else if (rename(c->old, full) != 0) {
                char msg[400];
                snprintf(msg, sizeof(msg), "%s: %s", base_name(c->old), strerror(errno));
                dlg_message("Cannot rename", msg);
            } else {
                snprintf(fm.pending_select, sizeof(fm.pending_select), "%s", text);
                fm_reload();
            }
        }
    }
    free(c);
}

void ops_rename(void)
{
    int i = view_selected_index();
    if (i < 0 || view_count_selected() > 1)
        return;
    struct ren_ctx *c = malloc(sizeof(*c));
    if (!c)
        return;
    join_path(c->old, sizeof(c->old), fm.path, fm.ents[i].name);
    dlg_input("Rename", "New name", fm.ents[i].name, "Rename", rename_done, c);
}

void ops_properties(void)
{
    int i = view_selected_index();
    if (i < 0 || view_count_selected() == 0) {
        dlg_properties(fm.path);
        return;
    }
    char *p = view_path_of(i);
    if (p)
        dlg_properties(p);
    free(p);
}

bool ops_dir_size(const char *path, uint64_t *bytes, uint64_t *files, uint64_t *dirs, volatile bool *cancel)
{
    struct stat st;
    if (*cancel || lstat(path, &st) != 0)
        return false;
    if (!S_ISDIR(st.st_mode)) {
        *bytes += (uint64_t)st.st_size;
        (*files)++;
        return true;
    }
    (*dirs)++;
    DIR *d = opendir(path);
    if (!d)
        return false;
    struct dirent *de;
    while ((de = readdir(d)) && !*cancel) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char sub[FM_PATH];
        join_path(sub, sizeof(sub), path, de->d_name);
        ops_dir_size(sub, bytes, files, dirs, cancel);
    }
    closedir(d);
    return true;
}
