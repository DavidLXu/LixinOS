// Single foreground desktop. The backing image stays cursor-free; pointer motion
// transfers just the old and new cursor rectangles to the active QEMU pane.
#include "../console.h"
#include "../tcc/lixin.h"
#include <stdint.h>
#include <stddef.h>
#ifndef __TINYC__
#include <setjmp.h>
#define WAITING (*(jmp_buf *)0x443d7000UL)
#endif
#include <string.h>

enum { PAGE = 9, ROW = 30, CUR_W = 15, CUR_H = 20, TITLE = 44, FILE_H = 80 + ROW * PAGE + 41 };
enum {
    WALL = 0x26494c,
    PAPER = 0xf9f9f4,
    INK = 0x233c3d,
    MUTED = 0x526c6d,
    LINE = 0xd9e1da,
    BAR = 0xe8eee8,
    GOLD = 0xffcf75,
    OCHRE = 0xbc873f
};
static const char han[] = "文件夹上一级名称类型大小空目录页字节本编辑器终端";
#ifdef __TINYC__
static unsigned char *desktop_font;
void *malloc(unsigned long);
static int load_font(void) {
    const char *path = "/src/gui/menlo.gray";
    long size = lx_stat(path);
    if (size != (95 + (sizeof han - 1) / 3) * 18 * 22) return -1;
    int fd = lx_open(path);
    if (fd < 0) return -1;
    desktop_font = malloc(size);
    long offset = 0;
    while (desktop_font && offset < size) {
        int n = lx_read(fd, desktop_font+offset, size-offset);
        if (n <= 0) break;
        offset += n;
    }
    lx_close(fd);
    return offset == size ? 0 : -1;
}
#else
extern const unsigned char desktop_font[];
#endif
typedef struct {
    char name[513];
    long size;
} Entry;
typedef struct {
    uint32_t *pixels;
    int w, h, left, top, width, open, page, count, more, failed;
    int mx, my, inside, down;
    int naming, terminals, selected, dialog_x, dialog_y;
    int dragging, grab_x, grab_y;
    char filename[513], message[96];
    char path[513];
    Entry rows[PAGE];
} Desktop;

static int min(int a, int b) { return a < b ? a : b; }
static void box(Desktop *d, int x, int y, int w, int h, uint32_t color) {
    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i)
            d->pixels[j * d->w + i] = color;
}
static uint32_t blend(uint32_t bg, uint32_t fg, unsigned a) {
    unsigned rb = (((bg & 0xff00ff) * (256 - a) + (fg & 0xff00ff) * a) >> 8) & 0xff00ff;
    unsigned g = (((bg & 0xff00) * (256 - a) + (fg & 0xff00) * a) >> 8) & 0xff00;
    return rb | g;
}
static void label(Desktop *d, int x, int y, int room, const char *s, uint32_t color) {
    while (*s) {
        unsigned char b = *s++;
        int glyph = '?' - 32, advance = 9;
        if (b >= 32 && b < 127)
            glyph = b - 32;
        else if (b >= 128) {
            const char *first = s - 1;
            while (((unsigned char)*s & 0xc0) == 0x80)
                ++s;
            if (s - first == 3) {
                for (int i = 0; han[i]; i += 3)
                    if (!memcmp(first, han + i, 3)) {
                        glyph = 95 + i / 3;
                        advance = 18;
                        break;
                    }
            }
        }
        if (room < advance)
            break;
        const unsigned char *alpha = desktop_font + glyph * 18 * 22;
        for (int j = 0; j < 22; ++j)
            for (int i = 0; i < advance; ++i) {
                uint32_t *p = d->pixels + (y + j) * d->w + x + i;
                *p = blend(*p, color, alpha[j * 18 + i]);
            }
        x += advance;
        room -= advance;
    }
}
static void folder(Desktop *d, int x, int y, int unit) {
    box(d, x, y, 8 * unit, 3 * unit, OCHRE);
    box(d, x, y + 2 * unit, 18 * unit, 10 * unit, OCHRE);
    box(d, x, y + 4 * unit, 18 * unit, 9 * unit, GOLD);
}
static void number(char *out, unsigned long n) {
    char rev[24];
    int i = 0;
    do {
        rev[i++] = '0' + n % 10;
        n /= 10;
    } while (n);
    while (i)
        *out++ = rev[--i];
    *out = 0;
}
static void load(Desktop *d) {
    d->failed = 0;
    d->count = 0;
    d->more = 0;
    for (int i = 0; i <= PAGE; ++i) {
        char extra[513];
        char *name = i == PAGE ? extra : d->rows[i].name;
        long size = lx_dir(d->path, d->page * PAGE + i, name);
        if (size == LX_END)
            break;
        if (size < LX_DIR) {
            d->failed = 1;
            break;
        }
        if (i == PAGE) {
            d->more = 1;
            break;
        }
        d->rows[i].size = size;
        ++d->count;
    }
}
static int present(uint32_t *p, int x, int y, int w, int h) {
    uintptr_t *m = (uintptr_t *)0x443d8000;
    m[0] = (uintptr_t)p;
    m[1] = x;
    m[2] = y;
    m[3] = w;
    m[4] = h;
    return console_call(20, 0) == 0;
}
static void cursor(Desktop *d, int draw) {
    if (!d->inside)
        return;
    int w = min(CUR_W, d->w - d->mx), h = min(CUR_H, d->h - d->my);
    uint32_t *tile = d->pixels + d->w * d->h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t c = d->pixels[(d->my + y) * d->w + d->mx + x];
            if (draw && y < 16 && x <= y * 2 / 3)
                c = (x == 0 || x == y * 2 / 3 || y == 15) ? INK : 0xffffff;
            if (draw && y >= 12 && y < 20 && x >= y / 3 && x <= y / 3 + 2)
                c = x == y / 3 ? INK : 0xffffff;
            tile[y * w + x] = c;
        }
    present(tile, d->mx, d->my, w, h);
}
// Shared chrome. The popup host copies this same title strip for EL0 text windows.
static void titlebar(Desktop *d, int x, int y, int w, const char *title, int minimize) {
    box(d, x, y, w, TITLE, BAR);
    label(d, x+14, y+11, w-100, title, INK);
    int right = x+w;
    box(d, right-35, y+8, 28, 28, PAPER);
    for (int i=0; i<10; ++i) {
        box(d, right-26+i, y+17+i, 2, 2, INK);
        box(d, right-17-i, y+17+i, 2, 2, INK);
    }
    if (minimize) {
        box(d, right-68, y+8, 28, 28, PAPER);
        box(d, right-60, y+25, 12, 2, INK);
    }
}
static int close_hit(int x, int y, int width) {
    return x >= width-35 && x < width-7 && y >= 8 && y < 36;
}
static int clamp(int value, int high) { return value < 0 ? 0 : min(value, high); }
static void render(Desktop *d) {
    box(d, 0, 0, d->w, d->h, WALL);
    folder(d, 31, 25, 3);
    label(d, 40, 69, 40, "文件", 0xffffff);
    box(d, 131, 25, 34, 39, PAPER);
    for (int i = 0; i < 3; ++i) box(d, 137, 34 + i*8, 22-i*3, 2, MUTED);
    label(d, 112, 69, 78, "文本编辑器", 0xffffff);
    box(d, 213, 26, 52, 37, INK);
    box(d, 215, 28, 48, 33, 0x192527);
    label(d, 220, 32, 32, ">_", GOLD);
    label(d, 222, 69, 42, "终端", 0xffffff);
    box(d, 20, 112, 74, 38, BAR);
    label(d, 35, 120, 52, "Links", INK);
    if (d->w >= 640) {
        box(d, 470, 25, 45, 40, PAPER);
        label(d, 481, 34, 28, "+-", INK);
        label(d, 452, 69, 90, "Calculator", 0xffffff);
        box(d, 570, 25, 32, 40, PAPER);
        for (int i=0; i<3; ++i) box(d, 575, 34+i*7, 21, 2, MUTED);
        label(d, 558, 69, 64, "Editor", 0xffffff);
    }
    if (d->terminals) {
        char id[24];
        number(id, d->selected);
        box(d, 292, 26, 153, 36, INK);
        label(d, 300, 33, 20, "<", GOLD);
        label(d, 326, 33, 58, "Shell", PAPER);
        label(d, 382, 33, 36, id, PAPER);
        label(d, 426, 33, 18, ">", GOLD);
    }
    if (d->open) {
        int l = d->left, t = d->top, w = d->width;
        int bottom = t + 80 + ROW * PAGE;
        box(d, l + 4, t + 5, w, bottom - t + 41, 0x1c383b);
        box(d, l, t, w, bottom - t + 41, PAPER);
        titlebar(d, l, t, w, "", 0);
        folder(d, l + 14, t + 13, 1);
        const char *path = d->path;
        int max = (w - 150) / 9;
        if ((int)strlen(path) > max) {
            path += strlen(path) - max;
            while (((unsigned char)*path & 0xc0) == 0x80)
                ++path;
            label(d, l + 40, t + 11, 27, "...", INK);
            label(d, l + 67, t + 11, w - 185, path, INK);
        } else
            label(d, l + 40, t + 11, w - 145, path, INK);
        box(d, l + w - 102, t + 8, 64, 28, PAPER);
        label(d, l + w - 97, t + 11, 54, "上一级", strcmp(d->path, "/") ? INK : MUTED);
        label(d, l + 15, t + 51, 70, "名称", MUTED);
        label(d, l + w - 176, t + 51, 70, "类型", MUTED);
        label(d, l + w - 95, t + 51, 70, "大小", MUTED);
        box(d, l, t + 77, w, 1, LINE);
        for (int i = 0; i < d->count; ++i) {
            int y = t + 80 + i * ROW;
            Entry *e = &d->rows[i];
            if (e->size == LX_DIR)
                folder(d, l + 15, y + 8, 1);
            else {
                box(d, l + 17, y + 6, 12, 16, 0x94adaf);
                box(d, l + 19, y + 9, 8, 1, PAPER);
            }
            label(d, l + 42, y + 3, w - 230, e->name, INK);
            label(d, l + w - 176, y + 3, 70, e->size == LX_DIR ? "文件夹" : "文件", MUTED);
            if (e->size >= 0) {
                char size[30];
                number(size, e->size);
                strcat(size, " B");
                label(d, l + w - 95, y + 3, 92, size, INK);
            }
        }
        if (d->failed)
            label(d, l + 16, t + 90, w - 32, "Directory unavailable", MUTED);
        else if (!d->count)
            label(d, l + 16, t + 90, w - 32, "空目录", MUTED);
        box(d, l, bottom, w, 1, LINE);
        char page[24];
        number(page, (unsigned)d->page + 1);
        label(d, l + 16, bottom + 9, 50, "页", MUTED);
        label(d, l + 40, bottom + 9, 72, page, INK);
        label(d, l + w - 80, bottom + 9, 18, "<", d->page ? INK : LINE);
        label(d, l + w - 33, bottom + 9, 18, ">", d->more ? INK : LINE);
    }
    if (d->naming) {
        int x = d->dialog_x, y = d->dialog_y;
        box(d, x, y, 440, 120, PAPER);
        titlebar(d, x, y, 440, "文本编辑器", 0);
        box(d, x+12, y+50, 416, 28, BAR);
        size_t n = strlen(d->filename);
        label(d, x+18, y+53, 398, d->filename+(n>42?n-42:0), INK);
        label(d, x+14, y+87, 410, "Enter: open or create     Esc: cancel", MUTED);
    }
    if (d->message[0]) {
        box(d, 8, d->h-30, d->w-16, 28, INK);
        label(d, 18, d->h-27, d->w-36, d->message, PAPER);
    }
    present(d->pixels, 0, 0, d->w, d->h);
    cursor(d, 1);
}
static int change_page(Desktop *d, int next) {
    if (next ? !d->more : !d->page)
        return 0;
    d->page += next ? 1 : -1;
    load(d);
    return 1;
}
// A cooperative yield lets the kernel run the selected EL0 application. The
// desktop and its private drawing arena survive until that application returns.
static void launch(Desktop *d, int action) {
    render(d);
    cursor(d, 0);
    // The physical display retains the clean desktop behind the new window.
    // Its host takes the title strip from our off-screen drawing buffer.
    int width = min(d->w / 11 - 8, 76) * 11 + 12;
    titlebar(d, 0, 0, width, action == -1 ? "文本编辑器" : "终端", action != -1);
    volatile int *mail = (volatile int *)0x443d8000;
    mail[0] = action;
    mail[1] = width;
    strcpy((char *)(mail+2), action < 0 ? d->filename : "");
#ifdef __TINYC__
    lx_agent_yield();
#else
    if (!setjmp(WAITING)) {
        console_call(32, 0);
        __builtin_unreachable();
    }
#endif
    d->terminals = mail[0];
    if (!action || d->selected > d->terminals) d->selected = d->terminals;
    unsigned n = 0;
    while (n + 1 < sizeof(d->message) && ((char *)(mail+2))[n]) {
        d->message[n] = ((char *)(mail+2))[n]; ++n;
    }
    d->message[n] = 0;
    if (d->open) load(d);
}
static int click(Desktop *d, int x, int y) {
    if (d->naming) {
        int dx=x-d->dialog_x, dy=y-d->dialog_y;
        if (close_hit(dx,dy,440)) { d->naming=0; return 1; }
        if (dx>=0 && dx<440 && dy>=0 && dy<TITLE) {
            d->dragging=2; d->grab_x=dx; d->grab_y=dy;
        }
        return 0;
    }
    d->message[0] = 0;
    int over_files = d->open && x>=d->left && x<d->left+d->width &&
                     y>=d->top && y<d->top+FILE_H;
    if (!over_files && x>=20 && x<94 && y>=112 && y<150) { strcpy(d->filename,"/bin/links"); launch(d,-2); return 1; }
    if (!over_files && y >= 18 && y < 96) {
        if (x >= 105 && x < 195) {
            strcpy(d->filename, d->open ? d->path : "/");
            size_t n = strlen(d->filename);
            if (n < 512 && d->filename[n-1] != '/') strcat(d->filename, "/");
            d->naming = 1;
            return 1;
        }
        if (x >= 201 && x < 280) { launch(d, 0); return 1; }
        if (d->w >= 640 && x >= 452 && x < 632) {
            strcpy(d->filename, x < 546 ? "/bin/calculator" : "/bin/text-editor");
            launch(d, -2);
            return 1;
        }
        if (d->terminals && x >= 292 && x < 445 && y < 63) {
            if (x < 320) d->selected = d->selected > 1 ? d->selected-1 : d->terminals;
            else if (x >= 415) d->selected = d->selected % d->terminals + 1;
            else launch(d, d->selected);
            return 1;
        }
    }
    if (!d->open) {
        if (x < 18 || x >= 102 || y < 17 || y >= 98)
            return 0;
        d->open = 1;
        strcpy(d->path, "/");
        d->page = 0;
        load(d);
        return 1;
    }
    x -= d->left;
    y -= d->top;
    if (x < 0 || x >= d->width || y < 0)
        return 0;
    if (close_hit(x,y,d->width)) {
        d->open = 0;
        return 1;
    }
    if (y >= 8 && y < 36 && x >= d->width - 102 && x < d->width - 38) {
        char *slash = strrchr(d->path, '/');
        if (slash == d->path)
            d->path[1] = 0;
        else
            *slash = 0;
    } else if (y < TITLE) {
        d->dragging=1; d->grab_x=x; d->grab_y=y;
        return 0;
    } else if (y >= 80 && y < 80 + ROW * d->count) {
        Entry *e = &d->rows[(y - 80) / ROW];

        size_t n = strlen(d->path);
        if (n + (n > 1) + strlen(e->name) >= sizeof(d->path))
            return 0;
        if (e->size != LX_DIR) {
            strcpy(d->filename, d->path);
            if (n > 1) strcat(d->filename, "/");
            strcat(d->filename, e->name);
            int app = 0;
            size_t filename_length = strlen(e->name);
            if (e->size >= 16 && filename_length >= 4
                && !strcmp(e->name + filename_length - 4, ".app")) {
                char magic[4];
                int fd = lx_open(d->filename);
                if (fd >= 0) {
                    app = lx_read(fd, magic, sizeof(magic)) == sizeof(magic)
                          && !memcmp(magic, "LXE1", sizeof(magic));
                    lx_close(fd);
                }
            }
            launch(d, app ? -2 : -1);
            return 1;
        }
        if (n > 1)
            strcat(d->path, "/");
        strcat(d->path, e->name);
    } else if (y >= 80 + ROW * PAGE && y < 121 + ROW * PAGE && x >= d->width - 100) {
        return change_page(d, x >= d->width - 53);
    } else
        return 0;
    d->page = 0;
    load(d);
    return 1;
}
#ifdef __TINYC__
int main(int argc, char **argv) {
    (void)argv;
    if (load_font() < 0) { lx_error("desktop: could not load font\n"); return 1; }
    unsigned len = argc - 1;
#else
__attribute__((section(".text.start"), noreturn)) void _start(const char *cwd, const char *args,
                                                              unsigned long len, unsigned long flags) {
    if (flags == 65536) longjmp(WAITING, 1);
    (void)cwd;
    (void)args;
#endif
    if (len) {
        lx_error("usage: desktop\n");
        finish(2);
    }
    long size = console_call(19, 0);
    if (size <= 0) {
        lx_error("desktop: open a QEMU graphical window with at least 42 columns and 20 rows per pane\n");
        finish(1);
    }
    Desktop d = {0};
    d.w = size & 65535;
    d.h = size >> 16;
    d.pixels = (uint32_t *)0x68000000;
    d.width = min(d.w - 32, 600);
    d.left = (d.w - d.width) / 2;
    d.top = (d.h - 391) / 2;
    d.dialog_x=(d.w-440)/2;
    d.dialog_y=172;
    d.mx = d.w / 2;
    d.my = d.h / 2;
    d.inside = 1;
    render(&d);
    for (;;) {
        int key = console_call(14, 8);
        if (d.naming && key >= 0 && key < 256) {
            size_t n = strlen(d.filename);
            if (key == 27) d.naming = 0;
            else if (key == 8 || key == 127) { if (n) d.filename[n-1] = 0; }
            else if ((key == 13 || key == 10) && n) {
                d.naming = 0;
                launch(&d, -1);
            } else if (key >= 32 && key < 127 && n < 512) {
                d.filename[n] = key;
                d.filename[n+1] = 0;
            }
            render(&d);
        } else if (key == 27) {
            if (!d.terminals || d.message[0] == '!') finish(0);
            strcpy(d.message, "! Open shells will close. Esc again to quit.");
            render(&d);
        }
        long e;
        while ((e = console_call(21, 0)) >= 0) {
            if (e & (1 << 28)) {
                d.dragging = d.down = 0;
                continue;
            }
            cursor(&d, 0);
            d.mx = e & 4095;
            d.my = (e >> 12) & 4095;
            d.inside = (e >> 25) & 1;
            int down = (e >> 24) & 1, changed = 0;
            if (d.dragging) {
                int dialog = d.dragging == 2;
                int *x = dialog ? &d.dialog_x : &d.left, *y = dialog ? &d.dialog_y : &d.top;
                int nx=clamp(d.mx-d.grab_x, d.w-(dialog ? 440 : d.width+4));
                int ny=clamp(d.my-d.grab_y, d.h-(dialog ? 120 : FILE_H+5));
                changed = *x != nx || *y != ny;
                *x=nx; *y=ny;
                if (!down) d.dragging=0;
            } else if (d.inside && down && !d.down)
                changed = click(&d, d.mx, d.my);
            else if (!d.naming && d.open && d.inside && (e & (3 << 26)))
                changed = change_page(&d, (e & (1 << 27)) != 0);
            d.down = down;
            if (changed)
                render(&d);
            else
                cursor(&d, 1);
        }
    }
}
