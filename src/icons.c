#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "fm.h"

static const struct
{
    const char *ext;
    int kind;
    const char *type;
} exts[] = {
    {"txt", IK_TEXT, "Text document"},
    {"md", IK_TEXT, "Markdown document"},
    {"log", IK_TEXT, "Log file"},
    {"conf", IK_TEXT, "Configuration"},
    {"ini", IK_TEXT, "Configuration"},
    {"cfg", IK_TEXT, "Configuration"},
    {"json", IK_TEXT, "JSON data"},
    {"xml", IK_TEXT, "XML data"},
    {"csv", IK_TEXT, "CSV table"},
    {"c", IK_CODE, "C source"},
    {"h", IK_CODE, "C header"},
    {"cpp", IK_CODE, "C++ source"},
    {"hpp", IK_CODE, "C++ header"},
    {"cc", IK_CODE, "C++ source"},
    {"py", IK_CODE, "Python script"},
    {"sh", IK_CODE, "Shell script"},
    {"asm", IK_CODE, "Assembly source"},
    {"s", IK_CODE, "Assembly source"},
    {"rs", IK_CODE, "Rust source"},
    {"js", IK_CODE, "JavaScript"},
    {"html", IK_CODE, "HTML document"},
    {"css", IK_CODE, "Stylesheet"},
    {"mk", IK_CODE, "Makefile"},
    {"ld", IK_CODE, "Linker script"},
    {"png", IK_IMAGE, "PNG image"},
    {"jpg", IK_IMAGE, "JPEG image"},
    {"jpeg", IK_IMAGE, "JPEG image"},
    {"bmp", IK_IMAGE, "Bitmap image"},
    {"gif", IK_IMAGE, "GIF image"},
    {"svg", IK_IMAGE, "SVG image"},
    {"ppm", IK_IMAGE, "PPM image"},
    {"ico", IK_IMAGE, "Icon"},
    {"webp", IK_IMAGE, "WebP image"},
    {"mp3", IK_AUDIO, "MP3 audio"},
    {"wav", IK_AUDIO, "WAV audio"},
    {"ogg", IK_AUDIO, "Ogg audio"},
    {"flac", IK_AUDIO, "FLAC audio"},
    {"gz", IK_ARCHIVE, "Gzip archive"},
    {"tgz", IK_ARCHIVE, "Tar.gz archive"},
    {"tar", IK_ARCHIVE, "Tar archive"},
    {"zip", IK_ARCHIVE, "Zip archive"},
    {"xz", IK_ARCHIVE, "XZ archive"},
    {"bz2", IK_ARCHIVE, "Bzip2 archive"},
    {"7z", IK_ARCHIVE, "7-Zip archive"},
    {"iso", IK_ARCHIVE, "Disk image"},
    {"img", IK_ARCHIVE, "Disk image"},
    {"o", IK_FILE, "Object file"},
    {"a", IK_FILE, "Static library"},
    {"so", IK_FILE, "Shared library"},
    {"bin", IK_FILE, "Binary data"},
    {"elf", IK_EXEC, "Executable"},
    {"desktop", IK_TEXT, "Desktop entry"},
};

static bool is_package(const char *name)
{
    size_t n = strlen(name);
    return n > 11 && strcasecmp(name + n - 11, ".pkg.tar.gz") == 0;
}

const char *ext_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1])
        return "";
    return dot + 1;
}

static int ext_index(const char *name)
{
    const char *x = ext_of(name);
    if (!*x)
        return -1;
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
        if (strcasecmp(exts[i].ext, x) == 0)
            return (int)i;
    return -1;
}

int icon_kind_for(const struct entry *e)
{
    if (e->dir)
        return IK_FOLDER;
    if (is_package(e->name))
        return IK_PACKAGE;
    int i = ext_index(e->name);
    if (i >= 0)
        return exts[i].kind;
    if (e->stated && (e->mode & 0111) && S_ISREG(e->mode))
        return IK_EXEC;
    return IK_FILE;
}

const char *type_name_for(const struct entry *e)
{
    if (e->dir)
        return e->link ? "Link to folder" : "Folder";
    if (is_package(e->name))
        return "PureC package";
    int i = ext_index(e->name);
    if (i >= 0)
        return exts[i].type;
    if (e->stated)
    {
        if (S_ISCHR(e->mode))
            return "Character device";
        if (S_ISBLK(e->mode))
            return "Block device";
        if (S_ISFIFO(e->mode))
            return "Pipe";
        if (S_ISSOCK(e->mode))
            return "Socket";
        if (e->mode & 0111)
            return "Executable";
    }
    return e->link ? "Link" : "File";
}

static void page(struct pg_canvas *c, int x, int y, int s, uint32_t fill, uint32_t edge)
{
    int w = s * 3 / 4, h = s, px = x + (s - w) / 2;
    int f = s / 4;
    pgc_rrect(c, px, y, w, h, s / 10 + 1, edge, 255);
    pgc_rrect(c, px + 1, y + 1, w - 2, h - 2, s / 10, fill, 255);
    pgc_fill(c, px + w - f - 1, y, f + 1, f + 1, pgc_mix(fill, edge, 70));
    pgc_line(c, (float)(px + w - f - 1), (float)y, (float)(px + w - 1), (float)(y + f), 1.0f, edge);
    pgc_line(c, (float)(px + w - f - 1), (float)y, (float)(px + w - f - 1), (float)(y + f), 1.0f, edge);
    pgc_line(c, (float)(px + w - f - 1), (float)(y + f), (float)(px + w - 1), (float)(y + f), 1.0f, edge);
}

static void text_lines(struct pg_canvas *c, int x, int y, int s, uint32_t col, int count)
{
    int w = s * 3 / 4, px = x + (s - w) / 2 + s / 8;
    int lw = w - s / 4;
    int top = y + s * 40 / 100;
    int step = s / 8 > 2 ? s / 8 : 3;
    for (int i = 0; i < count; i++)
        pgc_fill(c, px, top + i * step, i == count - 1 ? lw * 2 / 3 : lw, s >= 40 ? 2 : 1, col);
}

static void draw_folder(struct pg_canvas *c, int x, int y, int s)
{
    const struct pg_theme *t = pg_theme();
    uint32_t back = pgc_mix(t->warning, 0x000000u, 40);
    uint32_t front = t->warning;
    int h = s * 78 / 100, top = y + (s - h) / 2;
    int tab = s * 40 / 100;
    pgc_rrect(c, x, top, tab, h / 4, s / 12 + 1, back, 255);
    pgc_rrect(c, x, top + h / 8, s, h - h / 8, s / 10 + 1, back, 255);
    pgc_rrect_grad(c, x, top + h * 26 / 100, s, h - h * 26 / 100, s / 10 + 1, pgc_lighten(front, 24), front, false);
}

static void draw_drive(struct pg_canvas *c, int x, int y, int s)
{
    const struct pg_theme *t = pg_theme();
    int h = s * 55 / 100, top = y + (s - h) / 2;
    pgc_rrect(c, x, top, s, h, s / 8 + 1, t->text3, 255);
    pgc_rrect_grad(c, x + 1, top + 1, s - 2, h - 2, s / 8, t->surface3, t->surface2, false);
    pgc_fill(c, x + s / 8, top + h * 55 / 100, s / 2, 2, t->text3);
    pgc_circle(c, x + s - s / 5, top + h * 62 / 100, s / 14 + 1, t->success, 255);
}

static void draw_package(struct pg_canvas *c, int x, int y, int s)
{
    const struct pg_theme *t = pg_theme();
    uint32_t top = pgc_lighten(t->accent, 30);
    int m = s / 10;
    pgc_rrect(c, x + m, y + m, s - 2 * m, s - 2 * m, s / 9 + 1, t->accent, 255);
    pgc_rrect_grad(c, x + m, y + m, s - 2 * m, (s - 2 * m) * 40 / 100, s / 9 + 1, top, t->accent, false);
    pgc_fill(c, x + s / 2 - 1, y + m, 3, s - 2 * m, pgc_mix(t->accent, 0x000000u, 90));
    pgc_fill(c, x + m + 2, y + m + (s - 2 * m) * 40 / 100, s - 2 * m - 4, 1, pgc_mix(t->accent, 0x000000u, 90));
}

static void draw_exec(struct pg_canvas *c, int x, int y, int s)
{
    const struct pg_theme *t = pg_theme();
    int m = s / 12;
    pgc_rrect(c, x + m, y + m, s - 2 * m, s - 2 * m, s / 6 + 1, pgc_mix(t->surface3, 0x000000u, 60), 255);
    pgc_rrect(c, x + m + 1, y + m + 1, s - 2 * m - 2, s - 2 * m - 2, s / 6, 0x1E1E2Eu, 255);
    float w = s >= 40 ? 2.0f : 1.4f;
    float cx = (float)x + (float)s * 0.32f, cy = (float)y + (float)s * 0.5f, d = (float)s * 0.12f;
    pgc_line(c, cx - d, cy - d, cx + d, cy, w, t->success);
    pgc_line(c, cx + d, cy, cx - d, cy + d, w, t->success);
    pgc_line(c, cx + d + (float)s * 0.08f, cy + d, cx + d + (float)s * 0.28f, cy + d, w, t->success);
}

static void draw_archive(struct pg_canvas *c, int x, int y, int s)
{
    const struct pg_theme *t = pg_theme();
    uint32_t body = pgc_mix(t->warning, t->text2, 90);
    page(c, x, y, s, pgc_lighten(body, 30), pgc_mix(body, 0x000000u, 70));
    int cx = x + s / 2 - 1;
    int step = s / 8 > 2 ? s / 8 : 3;
    for (int i = 0; i < 5; i++)
        pgc_fill(c, cx + (i & 1), y + 2 + i * step, 3, step - 1 > 0 ? step - 1 : 1, pgc_mix(body, 0x000000u, 120));
}

static void draw_image(struct pg_canvas *c, int x, int y, int s)
{
    const struct pg_theme *t = pg_theme();
    int h = s * 78 / 100, top = y + (s - h) / 2;
    pgc_rrect(c, x, top, s, h, s / 10 + 1, t->text3, 255);
    pgc_rrect_grad(c, x + 1, top + 1, s - 2, h - 2, s / 10, pgc_lighten(t->info, 40), t->info, false);
    pgc_circle(c, x + s * 70 / 100, top + h * 32 / 100, s / 9 + 1, 0xFFE9A8u, 255);
    pgc_line(c, (float)(x + 2), (float)(top + h - 3), (float)(x + s * 36 / 100), (float)(top + h * 40 / 100), (float)(s / 7 + 1), t->success);
    pgc_line(c, (float)(x + s * 28 / 100), (float)(top + h - 3), (float)(x + s * 62 / 100), (float)(top + h * 55 / 100), (float)(s / 7 + 1), pgc_mix(t->success, 0x000000u, 50));
}

static void draw_audio(struct pg_canvas *c, int x, int y, int s)
{
    const struct pg_theme *t = pg_theme();
    page(c, x, y, s, t->surface, t->text3);
    int cx = x + s / 2;
    pgc_circle(c, cx - s / 10, y + s * 68 / 100, s / 9 + 1, t->accent, 255);
    pgc_fill(c, cx - s / 10 + s / 9 - 1, y + s * 34 / 100, 2, s * 34 / 100, t->accent);
    pgc_line(c, (float)(cx - s / 10 + s / 9), (float)(y + s * 34 / 100), (float)(cx + s / 8), (float)(y + s * 42 / 100), 2.0f, t->accent);
}

static void draw_code(struct pg_canvas *c, int x, int y, int s)
{
    const struct pg_theme *t = pg_theme();
    page(c, x, y, s, t->surface, t->text3);
    float w = s >= 40 ? 2.0f : 1.3f;
    float cy = (float)y + (float)s * 0.66f, cx = (float)x + (float)s * 0.5f, d = (float)s * 0.11f;
    pgc_line(c, cx - d * 0.6f, cy - d, cx - d * 1.8f, cy, w, t->info);
    pgc_line(c, cx - d * 1.8f, cy, cx - d * 0.6f, cy + d, w, t->info);
    pgc_line(c, cx + d * 0.6f, cy - d, cx + d * 1.8f, cy, w, t->info);
    pgc_line(c, cx + d * 1.8f, cy, cx + d * 0.6f, cy + d, w, t->info);
}

void icon_draw(struct pg_canvas *c, int kind, bool link, int x, int y, int s)
{
    const struct pg_theme *t = pg_theme();
    switch (kind)
    {
    case IK_FOLDER:
        draw_folder(c, x, y, s);
        break;
    case IK_DRIVE:
        draw_drive(c, x, y, s);
        break;
    case IK_PACKAGE:
        draw_package(c, x, y, s);
        break;
    case IK_EXEC:
        draw_exec(c, x, y, s);
        break;
    case IK_ARCHIVE:
        draw_archive(c, x, y, s);
        break;
    case IK_IMAGE:
        draw_image(c, x, y, s);
        break;
    case IK_AUDIO:
        draw_audio(c, x, y, s);
        break;
    case IK_CODE:
        draw_code(c, x, y, s);
        break;
    case IK_TEXT:
        page(c, x, y, s, t->surface, t->text3);
        text_lines(c, x, y, s, t->text3, 4);
        break;
    default:
        page(c, x, y, s, t->surface, t->text3);
        break;
    }
    if (link)
    {
        int r = s / 5 + 3;
        int cx = x + r - 1, cy = y + s - r + 1;
        pgc_circle(c, cx, cy, r, t->accent, 255);
        float a = (float)r * 0.5f;
        pgc_line(c, (float)cx - a, (float)cy + a * 0.6f, (float)cx + a * 0.7f, (float)cy - a * 0.7f, 1.4f, t->on_accent);
        pgc_line(c, (float)cx + a * 0.7f, (float)cy - a * 0.7f, (float)cx + a * 0.7f, (float)cy, 1.4f, t->on_accent);
        pgc_line(c, (float)cx + a * 0.7f, (float)cy - a * 0.7f, (float)cx, (float)cy - a * 0.7f, 1.4f, t->on_accent);
    }
}

void format_size(uint64_t size, char *out, size_t cap)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    if (size < 1024)
    {
        snprintf(out, cap, "%llu B", (unsigned long long)size);
        return;
    }
    double v = (double)size;
    int u = 0;
    while (v >= 1024.0 && u < 4)
    {
        v /= 1024.0;
        u++;
    }
    int whole = (int)v;
    int frac = (int)((v - (double)whole) * 10.0);
    if (whole >= 100)
        snprintf(out, cap, "%d %s", whole, units[u]);
    else
        snprintf(out, cap, "%d.%d %s", whole, frac, units[u]);
}
