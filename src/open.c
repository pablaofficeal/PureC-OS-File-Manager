#include <ctype.h>
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "fm.h"

extern char **environ;

#define MAX_RULES 256

struct rule {
    char *exts;
    char *action;
};

static struct rule rules[MAX_RULES];
static int nrules;
static char *exec_action;
static char *default_action;

static const char *builtin[] = {
    "txt,md,log,conf,ini,cfg,json,xml,csv,c,h,cpp,hpp,cc,py,asm,s,rs,js,html,css,mk,ld,desktop,patch,diff,yml,yaml,toml,sh=term:vim %f",
    "pkg.tar.gz=pkg",
};

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

static void add_rule(const char *exts, const char *action)
{
    if (nrules >= MAX_RULES)
        return;
    rules[nrules].exts = strdup(exts);
    rules[nrules].action = strdup(action);
    if (rules[nrules].exts && rules[nrules].action)
        nrules++;
}

static void parse_line(char *line)
{
    char *s = trim(line);
    if (!*s || *s == '#')
        return;
    char *eq = strchr(s, '=');
    if (!eq)
        return;
    *eq = 0;
    char *key = trim(s), *val = trim(eq + 1);
    if (!*key || !*val)
        return;
    if (strcmp(key, "executable") == 0) {
        free(exec_action);
        exec_action = strdup(val);
    } else if (strcmp(key, "default") == 0) {
        free(default_action);
        default_action = strdup(val);
    } else
        add_rule(key, val);
}

static void load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f))
        parse_line(line);
    fclose(f);
}

void open_load_config(void)
{
    const char *home = getenv("HOME");
    if (home && *home) {
        char p[FM_PATH];
        snprintf(p, sizeof(p), "%s/.config/files/open.conf", home);
        load_file(p);
    }
    load_file("/etc/files/open.conf");
    for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++) {
        char *copy = strdup(builtin[i]);
        if (copy) {
            parse_line(copy);
            free(copy);
        }
    }
    if (!exec_action)
        exec_action = strdup("run");
}

static bool name_has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    if (e == 0 || n <= e + 1)
        return false;
    return name[n - e - 1] == '.' && strcasecmp(name + n - e, ext) == 0;
}

static const char *find_action(const char *name)
{
    for (int i = 0; i < nrules; i++) {
        char *list = strdup(rules[i].exts);
        if (!list)
            continue;
        char *save = list;
        bool hit = false;
        for (char *tok = strsep(&save, ","); tok; tok = strsep(&save, ","))
            if (name_has_ext(name, trim(tok))) {
                hit = true;
                break;
            }
        free(list);
        if (hit)
            return rules[i].action;
    }
    return NULL;
}

static char **split_args(const char *cmd, int *count)
{
    int cap = 8, n = 0;
    char **argv = calloc((size_t)cap + 1, sizeof(char *));
    if (!argv)
        return NULL;
    const char *p = cmd;
    while (*p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        char tok[FM_PATH + 64];
        size_t t = 0;
        char quote = 0;
        while (*p && (quote || !isspace((unsigned char)*p))) {
            if (quote) {
                if (*p == quote)
                    quote = 0;
                else if (t < sizeof(tok) - 1)
                    tok[t++] = *p;
            } else if (*p == '"' || *p == '\'')
                quote = *p;
            else if (t < sizeof(tok) - 1)
                tok[t++] = *p;
            p++;
        }
        tok[t] = 0;
        if (n == cap) {
            cap *= 2;
            char **g = realloc(argv, sizeof(char *) * ((size_t)cap + 1));
            if (!g)
                break;
            argv = g;
        }
        argv[n++] = strdup(tok);
    }
    argv[n] = NULL;
    *count = n;
    return argv;
}

static void free_args(char **argv)
{
    for (char **a = argv; a && *a; a++)
        free(*a);
    free(argv);
}

static void expand(const char *tmpl, const char *path, char *out, size_t cap)
{
    size_t o = 0;
    bool used = false;
    char q = strchr(path, '"') ? '\'' : '"';
    for (const char *p = tmpl; *p && o + 2 < cap; p++) {
        if (p[0] == '%' && p[1] == 'f') {
            int r = snprintf(out + o, cap - o, "%c%s%c", q, path, q);
            o += (size_t)(r > 0 ? r : 0);
            if (o >= cap)
                o = cap - 1;
            p++;
            used = true;
        } else
            out[o++] = *p;
    }
    out[o < cap ? o : cap - 1] = 0;
    if (!used && path[0]) {
        size_t len = strlen(out);
        snprintf(out + len, cap - len, " %c%s%c", q, path, q);
    }
}

static void spawn_exec(const char *cmd)
{
    int n;
    char **argv = split_args(cmd, &n);
    if (!argv || n == 0) {
        free_args(argv);
        return;
    }
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);
    if (rc != 0) {
        char msg[400];
        snprintf(msg, sizeof(msg), "%s: %s", argv[0], ops_strerror(rc));
        dlg_message("Cannot start program", msg);
    }
    free_args(argv);
}

static void spawn_terminal(const char *cmd, bool pause)
{
    size_t count = 0;
    while (environ && environ[count])
        count++;
    char **env = calloc(count + 5, sizeof(char *));
    if (!env)
        return;
    size_t n = 0;
    for (size_t i = 0; i < count; i++)
        if (strncmp(environ[i], "FILES_", 6) != 0 && strncmp(environ[i], "SHELL=", 6) != 0)
            env[n++] = environ[i];
    char run[FM_PATH * 2 + 32], cwd[FM_PATH + 16];
    snprintf(run, sizeof(run), "FILES_RUN=%s", cmd);
    snprintf(cwd, sizeof(cwd), "FILES_CWD=%s", fm.path);
    env[n++] = run;
    env[n++] = cwd;
    env[n++] = pause ? "FILES_PAUSE=1" : "FILES_NOPAUSE=1";
    env[n++] = "SHELL=/usr/bin/files";
    env[n] = NULL;
    char *argv[] = {"terminal", NULL};
    pid_t pid;
    int rc = posix_spawnp(&pid, "terminal", NULL, NULL, argv, env);
    free(env);
    if (rc != 0) {
        char msg[200];
        snprintf(msg, sizeof(msg), "Cannot start the terminal: %s", ops_strerror(rc));
        dlg_message("Files", msg);
    }
}

int open_trampoline(const char *cmd, bool pause)
{
    const char *cwd = getenv("FILES_CWD");
    if (cwd && *cwd && chdir(cwd) != 0)
        fprintf(stderr, "files: cannot enter %s\n", cwd);
    int n;
    char **argv = split_args(cmd, &n);
    int status = 0;
    if (argv && n > 0) {
        pid_t pid = fork();
        if (pid == 0) {
            execvp(argv[0], argv);
            fprintf(stderr, "%s: %s\n", argv[0], strerror(errno));
            _exit(127);
        }
        if (pid > 0)
            waitpid(pid, &status, 0);
        else
            fprintf(stderr, "files: fork failed\n");
    }
    free_args(argv);
    if (pause) {
        printf("\n[finished] press Enter to close\n");
        fflush(stdout);
        char c;
        while (read(0, &c, 1) == 1 && c != '\n')
            ;
    }
    return 0;
}

static void run_action(const char *action, const char *path);

struct pkg_ctx {
    char path[FM_PATH];
};

static void pkg_done(const char *text, void *data)
{
    struct pkg_ctx *c = data;
    if (text) {
        char cmd[FM_PATH * 2];
        expand("pkg -U %f", c->path, cmd, sizeof(cmd));
        spawn_terminal(cmd, true);
    }
    free(c);
}

static void run_action(const char *action, const char *path)
{
    char cmd[FM_PATH * 2];
    if (strcmp(action, "run") == 0) {
        expand("%f", path, cmd, sizeof(cmd));
        spawn_terminal(cmd, true);
    } else if (strcmp(action, "pkg") == 0) {
        struct pkg_ctx *c = malloc(sizeof(*c));
        if (!c)
            return;
        snprintf(c->path, sizeof(c->path), "%s", path);
        char msg[FM_PATH + 100];
        snprintf(msg, sizeof(msg), "Install \"%s\" with pkg -U?", base_name(path));
        dlg_confirm("Install package", msg, "Install", false, pkg_done, c);
    } else if (strncmp(action, "term:", 5) == 0) {
        expand(action + 5, path, cmd, sizeof(cmd));
        spawn_terminal(cmd, false);
    } else if (strncmp(action, "termwait:", 9) == 0) {
        expand(action + 9, path, cmd, sizeof(cmd));
        spawn_terminal(cmd, true);
    } else if (strncmp(action, "exec:", 5) == 0) {
        expand(action + 5, path, cmd, sizeof(cmd));
        spawn_exec(cmd);
    }
}

void open_with_command(const char *cmd, const char *path)
{
    char out[FM_PATH * 2];
    expand(cmd, path, out, sizeof(out));
    spawn_terminal(out, false);
}

void open_path(const char *path, bool is_dir)
{
    if (is_dir) {
        fm_navigate(path, true);
        return;
    }
    const char *action = find_action(base_name(path));
    struct stat st;
    if (!action && stat(path, &st) == 0 && (st.st_mode & 0111) && exec_action)
        action = exec_action;
    if (!action)
        action = default_action;
    if (action)
        run_action(action, path);
    else
        dlg_open_with(path);
}

void open_entry(int i)
{
    if (i < 0 || i >= fm.n)
        return;
    char *p = view_path_of(i);
    if (!p)
        return;
    open_path(p, fm.ents[i].dir);
    free(p);
}
