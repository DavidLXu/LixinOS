// Single foreground desktop. The backing image stays cursor-free; pointer motion
// transfers just the old and new cursor rectangles to the active QEMU pane.
#include "../console.h"
#include "../file-time.h"
#include <stdlib.h>
#include "../tcc/lixin.h"
#include <stdint.h>
#include <stddef.h>
#ifndef __TINYC__
#include <setjmp.h>
#ifdef LX_NETSURF
#include <unistd.h>
static jmp_buf WAITING;
extern void lixin_initial_url(const char *url);
unsigned long desktop_thread_pointer;
void desktop_resume(void) { longjmp(WAITING, 1); }
#else
#define WAITING (*(jmp_buf *)0x443d7000UL)
#endif
#endif
#include <string.h>
#include "../gui/calculator-model.h"
#include "../gui/text-clipboard.h"
#include "../gui/widgets.h"
#include "../gui/directory-nav.h"
#include "../gui/commands.h"
#include "window-client-host.h"
#ifndef __TINYC__
#include "../links/unicode.h"
#endif
#include "../gui/han-font.h"
#include "../gui/mouse-cursor.h"
#ifdef __TINYC__
/* The in-guest desktop build compiles this single translation unit. */
#include "../gui/han-font.c"
#endif
#ifndef __TINYC__
#include "browser.h"
#endif

enum { DOCK = 76 };
/* The first three dock items correspond to the built-in windows. */
enum { FILE_WIN, WEB_WIN, CALC_WIN, FIRST_CLIENT, CLIENT_LIMIT = 4, APPS = FIRST_CLIENT + CLIENT_LIMIT };
enum { TERMINAL_ICON = 3, NANO_ICON, EDITOR_ICON, EYES_ICON, APPS_ICON, DOCK_ITEMS };
typedef struct {
    int x, y, w, h;
} Bounds;
typedef struct {
    Bounds rect, restored;
    int open, shown, maximized;
} Window;
enum { ROW = 30, TITLE = 44, FILE_FOOTER = 41 };
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
    if (size != (95 + (sizeof han - 1) / 3) * 18 * 22)
        return -1;
    int fd = lx_open(path);
    if (fd < 0)
        return -1;
    desktop_font = malloc(size);
    long offset = 0;
    while (desktop_font && offset < size) {
        int n = lx_read(fd, desktop_font + offset, size - offset);
        if (n <= 0)
            break;
        offset += n;
    }
    lx_close(fd);
    return offset == size ? 0 : -1;
}
#else
extern const unsigned char desktop_font[];
#endif
typedef struct {
    char *name;
    long size;
    unsigned modified;
    int chosen;
} Entry;
enum FileColumn { BY_NAME, BY_TYPE, BY_SIZE, BY_DATE, FILE_COLUMNS };
enum FileAction { F_OPEN, F_NEW_DIR, F_NEW_FILE, F_RENAME, F_COPY, F_CUT, F_PASTE,
                  F_TRASH, F_FIND, F_LOCATION, F_BACK, F_FORWARD, F_SHOW_TRASH, F_USE_EDITOR, F_USE_NANO, F_RESTORE, F_UNDO, F_ALL, F_ALWAYS_EDITOR, F_ALWAYS_NANO, F_PIN, F_PLACES, F_COUNT };
enum { EDITOR_NAME = 1, FILE_PROMPT = 2, WALL_PROMPT = FILE_PROMPT + F_COUNT, HISTORY = 12 };
enum { ANY, SINGLE, REGULAR, MANY, CLIP, PREVIOUS, NEXT, TRASH_ITEMS, UNDOABLE, LIST, EXTENSION };
static const UiCommand actions[F_COUNT] = {
    {"打开",13,SINGLE},{"新建文件夹",14,ANY},{"新建文件",0,ANY},{"重命名",18,SINGLE},
    {"复制",3,MANY},{"剪切",24,MANY},{"粘贴",22,CLIP},{"移到废纸篓",0,MANY},
    {"筛选名称",6,ANY},{"前往目录",12,ANY},{"后退",0,PREVIOUS},{"前进",0,NEXT},
    {"打开废纸篓",0,ANY},{"用文本编辑器打开",0,REGULAR},{"用 Nano 打开",0,REGULAR},
    {"恢复到原位置",0,TRASH_ITEMS},{"撤销移动或重命名",26,UNDOABLE},{"全选",1,LIST},
    {"始终用文本编辑器",0,EXTENSION},{"始终用 Nano",0,EXTENSION},
    {"收藏当前目录",4,ANY},{"显示位置",0,ANY}
};
typedef char FilePath[513];
#include "file-history.h"
typedef struct { FilePath source,target,origin; } FilePair;
typedef struct { FilePair *items; int count,action,conflict; } FileBatch;
enum AppGroup { GROUP_ALL, GROUP_SYSTEM, GROUP_EDIT, GROUP_TOOLS, GROUP_GAMES, GROUP_DEV, GROUP_OTHER, GROUP_COUNT };
static const char *const group_labels[GROUP_COUNT] = {"全部","系统","编辑","工具","游戏","开发","其他"};
enum LauncherIcon { ICON_FOLDER, ICON_GLOBE, ICON_CALC, ICON_TERMINAL, ICON_SHEET, ICON_PENCIL, ICON_STORE, ICON_GEAR, ICON_CHART, ICON_TILES, ICON_PLAY, ICON_CALENDAR, ICON_PIN, ICON_WATCH, ICON_PHOTO, ICON_MINE, ICON_MAHJONG, ICON_EYES, ICON_AGENT, ICON_CODE, ICON_COUNTER, ICON_WIDGETS, ICON_NUMBER };
static const struct { unsigned color; unsigned short rows[16]; } launcher_icons[] = {
    [ICON_FOLDER] = {0xe48c32, {0x0000,0x0000,0x0000,0x00fc,0x00fc,0x7ffe,0x7ffe,0x7ffe,0x7ffe,0x7ffe,0x7ffe,0x7ffe,0x7ffe,0x0000,0x0000,0x0000}},
    [ICON_GLOBE] = {0x247ab9, {0x0000,0x03c0,0x0ff0,0x1e78,0x342c,0x342c,0x6426,0x6426,0x7ffe,0x6426,0x342c,0x342c,0x1e78,0x0ff0,0x03c0,0x0000}},
    [ICON_CALC] = {0x378679, {0x0000,0x1ff8,0x1ff8,0x1818,0x1818,0x1818,0x1ff8,0x1ff8,0x1a58,0x1818,0x1818,0x1a58,0x1818,0x1ff8,0x1ff8,0x0000}},
    [ICON_TERMINAL] = {0x394f72, {0x0000,0x0000,0x0000,0x0000,0x0008,0x001c,0x0038,0x0070,0x00e0,0x0070,0x0038,0x001c,0x7f08,0x7f00,0x0000,0x0000}},
    [ICON_SHEET] = {0x9a556c, {0x0000,0x0000,0x3ffc,0x3ffc,0x300c,0x300c,0x3ffc,0x3ffc,0x300c,0x37ec,0x37ec,0x31ec,0x31ec,0x3ffc,0x3ffc,0x0000}},
    [ICON_PENCIL] = {0x526fc3, {0x0000,0x0800,0x1c00,0x3a00,0x7700,0xef80,0x5fc0,0x3fe0,0x1ff0,0x0ff8,0x07f8,0x03fc,0x01fc,0x00fe,0x0002,0x0000}},
    [ICON_STORE] = {0x3581c2, {0x0000,0x0180,0x03c0,0x0660,0x0660,0x0660,0x3ffc,0x3ffc,0x318c,0x300c,0x300c,0x300c,0x300c,0x3ffc,0x3ffc,0x0000}},
    [ICON_GEAR] = {0x647c8c, {0x0000,0x03c0,0x13c0,0x3bd0,0x1ff8,0x0ff0,0x7e7e,0x7c3e,0x7c3e,0x7e7e,0x0ff0,0x1ff8,0x3bd0,0x13c0,0x03c0,0x0000}},
    [ICON_CHART] = {0x38856f, {0x0000,0x0000,0x7000,0x7000,0x7000,0x7380,0x7380,0x7380,0x7380,0x739c,0x739c,0x739c,0x739c,0x739c,0x0000,0x0000}},
    [ICON_TILES] = {0x9e6199, {0x0000,0x0000,0x3e7c,0x3e7c,0x366c,0x3e7c,0x3e7c,0x0000,0x0000,0x3e7c,0x3e7c,0x366c,0x3e7c,0x3e7c,0x0000,0x0000}},
    [ICON_PLAY] = {0xd56356, {0x0000,0x0000,0x0010,0x0070,0x00f0,0x03f0,0x0ff0,0x1ff0,0x7ff0,0x1ff0,0x0ff0,0x03f0,0x00f0,0x0070,0x0010,0x0000}},
    [ICON_CALENDAR] = {0xd66257, {0x0000,0x0000,0x3ffc,0x3ffc,0x300c,0x300c,0x3ffc,0x3ffc,0x300c,0x37ec,0x37ec,0x31ec,0x31ec,0x3ffc,0x3ffc,0x0000}},
    [ICON_PIN] = {0x38997d, {0x0000,0x03c0,0x0ff0,0x1ff8,0x3e7c,0x3c3c,0x3c3c,0x3e7c,0x3ffc,0x1ff8,0x0ff0,0x07e0,0x07c0,0x0380,0x0380,0x0100}},
    [ICON_WATCH] = {0xc18336, {0x0000,0x03c0,0x03c0,0x0000,0x03c0,0x0ff0,0x1d38,0x1918,0x310c,0x310c,0x3f0c,0x300c,0x1818,0x1c38,0x0ff0,0x03c0}},
    [ICON_PHOTO] = {0x6175b8, {0x0000,0x0000,0x7ffe,0x7ffe,0x6006,0x6026,0x6806,0x6c06,0x7c46,0x7ee6,0x7ff6,0x7ffe,0x7ffe,0x7ffe,0x0000,0x0000}},
    [ICON_MINE] = {0x877195, {0x0000,0x0000,0x3e00,0x3e00,0x0300,0x03c0,0x07e0,0x0ff0,0x1fb8,0x1ff8,0x1ff8,0x1ff8,0x0ff0,0x07e0,0x03c0,0x0000}},
    [ICON_MAHJONG] = {0x288888, {0x0000,0x1ff8,0x1ff8,0x1818,0x1b18,0x1b18,0x1ff8,0x1ff8,0x1b18,0x1b18,0x1b18,0x1b18,0x1818,0x1ff8,0x1ff8,0x0000}},
    [ICON_EYES] = {0x825aa3, {0x0000,0x0000,0x0000,0x0000,0x3c3c,0x7e7e,0x7e7e,0xefef,0xc7c7,0xefef,0x7e7e,0x7e7e,0x3c3c,0x0000,0x0000,0x0000}},
    [ICON_AGENT] = {0x3b948c, {0x0000,0x0100,0x0300,0x0300,0x0300,0x7ffe,0x7ffe,0x6006,0x6c36,0x6c36,0x6006,0x7ffe,0x7ffe,0x0000,0x0000,0x0000}},
    [ICON_CODE] = {0x5f73a8, {0x0000,0x0000,0x0000,0x0020,0x0870,0x1c38,0x381c,0x700e,0xe004,0x700e,0x381c,0x1c38,0x0870,0x0020,0x0000,0x0000}},
    [ICON_COUNTER] = {0xad6b44, {0x0000,0x0000,0x0380,0x03c0,0x03e0,0x0340,0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,0x0300,0x0fe0,0x0fe0,0x0000}},
    [ICON_WIDGETS] = {0xb46f8c, {0x0000,0x0000,0x3e7c,0x3e7c,0x367c,0x3e7c,0x3e7c,0x0000,0x0000,0x3e7c,0x3e7c,0x3e6c,0x3e7c,0x3e7c,0x0000,0x0000}},
    [ICON_NUMBER] = {0xe2953d, {0x0000,0x0000,0x3e7c,0x3e7c,0x3e7c,0x3e7c,0x3e7c,0x0000,0x0000,0x3e7c,0x3e7c,0x3e7c,0x3e7c,0x3e7c,0x0000,0x0000}},
};
typedef struct { const char *file, *title; enum AppGroup group; enum LauncherIcon icon; } AppInfo;
static const AppInfo catalog[] = {
    {"app-store.win3","应用商店",GROUP_SYSTEM,ICON_STORE},
    {"settings.win3","系统设置",GROUP_SYSTEM,ICON_GEAR}, {"settings.win2","系统设置",GROUP_SYSTEM,ICON_GEAR},
    {"tasks.win2","任务管理器",GROUP_SYSTEM,ICON_CHART}, {"nautilus.win3","Nautilus",GROUP_SYSTEM,ICON_FOLDER},
    {"lite-xl.win3","Lite XL",GROUP_EDIT,ICON_PENCIL}, {"mousepad.win3","Mousepad",GROUP_EDIT,ICON_SHEET},
    {"physics.win3","物理仿真器",GROUP_TOOLS,ICON_CHART},
    {"video.win3","视频播放器",GROUP_TOOLS,ICON_PLAY}, {"calendar.win2","日历",GROUP_TOOLS,ICON_CALENDAR}, {"map.win2","地图",GROUP_TOOLS,ICON_PIN},
    {"stopwatch.win2","秒表",GROUP_TOOLS,ICON_WATCH}, {"ristretto.win3","Ristretto",GROUP_TOOLS,ICON_PHOTO},
    {"2048.win3","2048",GROUP_GAMES,ICON_NUMBER}, {"tetravex.win3","数字拼图",GROUP_GAMES,ICON_TILES},
    {"mines.win3","扫雷",GROUP_GAMES,ICON_MINE}, {"mahjongg.win3","麻将接龙",GROUP_GAMES,ICON_MAHJONG},
    {"nxeyes","Eyes",GROUP_GAMES,ICON_EYES}, {"agent.win3","Agent",GROUP_DEV,ICON_AGENT},
    {"gtk3-demo.win3","GTK 3 示例",GROUP_DEV,ICON_CODE}, {"counter.win","计数器示例",GROUP_DEV,ICON_COUNTER},
    {"widgets-demo.win3","控件示例",GROUP_DEV,ICON_WIDGETS}
};
static const AppInfo dock_apps[] = {
    {NULL,"文件",GROUP_SYSTEM,ICON_FOLDER}, {NULL,"浏览器",GROUP_TOOLS,ICON_GLOBE},
    {NULL,"计算器",GROUP_TOOLS,ICON_CALC}, {NULL,"终端",GROUP_DEV,ICON_TERMINAL},
    {NULL,"Nano",GROUP_EDIT,ICON_SHEET}, {NULL,"文本编辑器",GROUP_EDIT,ICON_PENCIL}
};
typedef struct { FilePath name; const AppInfo *info; int dock_id; } LauncherApp;
static const AppInfo *app_info(const char *name) {
    for (unsigned i=0; i<sizeof catalog/sizeof catalog[0]; ++i)
        if (!strcmp(name,catalog[i].file)) return &catalog[i];
    return NULL;
}
typedef struct {
    uint32_t *pixels;
    int w, h, first, count, failed;
    int capacity, sort_column, descending;
    Window windows[APPS];
    WindowClient *clients;
    char client_path[CLIENT_LIMIT][513];
    int wallpaper, wallpaper_menu, wallpaper_x, wallpaper_y;
    uint32_t *wall_image;
    FilePath wall_file;
    int client_grab;
    int menu, app_count, app_capacity, app_scroll;
    int app_press_x, app_press_y, app_press_scroll, app_dragged;
    enum AppGroup app_group;
    unsigned app_totals[GROUP_COUNT];
    LauncherApp *apps;
    int order[APPS], captured;
    char cwd[513];
    int mx, my, inside, down;
    int naming, terminals, selected, dialog_x, dialog_y;
    int dragging, grab_x, grab_y;
    char filename[513], message[96];
    char path[513];
    Entry *rows;
    int marked, action_menu, cut, history_at, history_count;
    UiText name;
    int name_offset, name_drag, range_anchor, chosen, modifiers;
    FilePath *clipboard;
    int clipboard_count;
    FileBatch pending,undo;
    long clicked_at;
    char filter[513];
    FileVisit history[HISTORY];
    UiPlaces *places;
    int places_open;
} Desktop;

static int min(int a, int b) { return a < b ? a : b; }
static void box(Desktop *d, int x, int y, int w, int h, uint32_t color) {
    for (int j = y < 0 ? 0 : y; j < min(d->h, y + h); ++j)
        for (int i = x; i < x + w; ++i)
            d->pixels[j * d->w + i] = color;
}
static uint32_t blend(uint32_t bg, uint32_t fg, unsigned a) {
    unsigned rb = (((bg & 0xff00ff) * (256 - a) + (fg & 0xff00ff) * a) >> 8) & 0xff00ff;
    unsigned g = (((bg & 0xff00) * (256 - a) + (fg & 0xff00) * a) >> 8) & 0xff00;
    return rb | g;
}
static int label_advance(unsigned code, const unsigned char **latin) {
    if (han_slot(code) >= 0) return 18;
    int width = lx_latin_glyph(code, 0, 1, latin);
    return width ? width : 9;
}
static int label_width(const char *s) {
    int width = 0;
    while (*s) width += label_advance(utf8_next(&s), NULL);
    return width;
}
static int name_hit(const Desktop *d, int x, int y) {
    return d->naming && x >= d->dialog_x + 12 && x < d->dialog_x + 428 &&
           y >= d->dialog_y + 50 && y < d->dialog_y + 78;
}
static int name_position(Desktop *d, int x) {
    const char *p = d->filename + d->name_offset;
    x -= d->dialog_x + 18;
    while (*p && x > 0) {
        const char *next = p;
        int advance = label_advance(utf8_next(&next), NULL);
        if (x < (advance + 1) / 2) break;
        x -= advance;
        p = next;
    }
    return p - d->filename;
}
static void name_begin(Desktop *d, int select_all) {
    ui_text_begin(&d->name,d->filename,select_all);
    d->name_offset = d->name_drag = 0;
}
static void label(Desktop *d, int x, int y, int room, const char *s, uint32_t color) {
    while (*s) {
        const char *first = s;
        unsigned code = utf8_next(&s);
        int glyph = code >= 32 && code < 127 ? (int)code - 32 : '?' - 32;
        const unsigned char *latin = NULL;
        int advance = label_advance(code, &latin);
        const unsigned char *bits = NULL;
        const unsigned char *pingfang = NULL;
        if (han_slot(code) >= 0) {
            pingfang = lx_pingfang_glyph(code, 1);
            if (!pingfang && s - first == 3) {
                for (size_t i = 0; i < sizeof(han) - 1; i += 3) {
                    if (!memcmp(first, han + i, 3)) {
                        glyph = 95 + i / 3;
                        break;
                    }
                }
            }
            if (!pingfang && glyph < 95)
                bits = lx_han_glyph(code);
        }
        if (room < advance)
            break;
        if (latin) {
            for (int j = y < -3 ? -y-3 : 0; j < min(16,d->h-y-3); ++j)
                for (int i = 0; i < min(18, advance); ++i) {
                    uint32_t *p = d->pixels + (y + j + 3) * d->w + x + i;
                    *p = blend(*p, color, latin[j * 20 + i + 2]);
                }
        } else if (pingfang) {
            for (int j = y < -3 ? -y-3 : 0; j < min(16,d->h-y-3); ++j)
                for (int i = 0; i < 16; ++i) {
                    uint32_t *p = d->pixels + (y + j + 3) * d->w + x + i;
                    *p = blend(*p, color, pingfang[j * 16 + i]);
                }
        } else if (bits) {
            for (int j = y < -3 ? -y-3 : 0; j < min(16,d->h-y-3); ++j)
                for (int i = 0; i < 16; ++i)
                    if (bits[j * 2 + i / 8] & (0x80 >> (i % 8)))
                        d->pixels[(y + j + 3) * d->w + x + i + 1] = color;
        } else {
            const unsigned char *alpha = desktop_font + glyph * 18 * 22;
            for (int j = y < 0 ? -y : 0; j < min(22,d->h-y); ++j)
                for (int i = 0; i < advance; ++i) {
                    uint32_t *p = d->pixels + (y + j) * d->w + x + i;
                    *p = blend(*p, color, alpha[j * 18 + i]);
                }
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
static int file_rows(Desktop *d) { return (d->windows[FILE_WIN].rect.h - FILE_FOOTER - 80) / ROW; }
static unsigned fold_ascii(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int name_matches(const char *name, const char *query) {
    for (; *name; ++name) {
        size_t i = 0;
        while (query[i] && fold_ascii(name[i]) == fold_ascii(query[i])) ++i;
        if (!query[i]) return 1;
    }
    return 0;
}
static const char *file_kind(const Entry *e) {
    if (e->size == LX_DIR) return "文件夹";
    const char *dot = strrchr(e->name,'.');
    return dot && dot != e->name && dot[1] ? dot+1 : "文件";
}
static int compare_text(const char *a, const char *b) {
    while (*a && fold_ascii(*a) == fold_ascii(*b)) { ++a; ++b; }
    return (int)fold_ascii(*a) - (int)fold_ascii(*b);
}
static int compare_names(const void *left, const void *right) {
    const Entry *a = left, *b = right;
    int order = compare_text(a->name,b->name);
    return order ? order : strcmp(a->name,b->name);
}
static int compare_types(const void *left, const void *right) {
    const Entry *a = left, *b = right;
    if ((a->size == LX_DIR) != (b->size == LX_DIR)) return a->size == LX_DIR ? -1 : 1;
    int order = compare_text(file_kind(a),file_kind(b));
    return order ? order : compare_names(a,b);
}
static int compare_sizes(const void *left, const void *right) {
    const Entry *a = left, *b = right;
    if (a->size != b->size) return a->size < b->size ? -1 : 1;
    return compare_names(a,b);
}
static int compare_dates(const void *left, const void *right) {
    const Entry *a = left, *b = right;
    if (a->modified != b->modified) return a->modified < b->modified ? -1 : 1;
    return compare_names(a,b);
}
static void sort_files(Desktop *d) {
    static int (*const compare[])(const void *,const void *) = {
        compare_names,compare_types,compare_sizes,compare_dates
    };
    if (d->count < 2) return;
    const char *selected = d->marked >= 0 ? d->rows[d->marked].name : NULL;
    const char *anchor = d->range_anchor>=0 ? d->rows[d->range_anchor].name : NULL;
    qsort(d->rows,d->count,sizeof(Entry),compare[d->sort_column]);
    if (d->descending) {
        for (int i = 0, j = d->count-1; i < j; ++i,--j) {
            Entry swap = d->rows[i];
            d->rows[i] = d->rows[j];
            d->rows[j] = swap;
        }
    }
    for (int i=0;i<d->count;++i) {
        if (d->rows[i].name==selected) d->marked=i;
        if (d->rows[i].name==anchor) d->range_anchor=i;
    }
}
static void clear_files(Desktop *d) {
    for (int i = 0; i < d->count; ++i) free(d->rows[i].name);
    d->count = 0;
    d->marked = d->range_anchor = -1;
    d->chosen=0;
    d->clicked_at = -1;
}
static int scroll_files(Desktop *d, int first) {
    int next = ui_list_top(first,d->count,file_rows(d));
    if (next == d->first) return 0;
    d->first = next;
    d->clicked_at = -1;
    return 1;
}
static void reveal_selection(Desktop *d) {
    scroll_files(d,ui_list_reveal(d->first,d->marked,d->count,file_rows(d)));
}
static void load(Desktop *d) {
    clear_files(d);
    d->failed = 0;
    for (unsigned index = 0;; ++index) {
        char name[513];
        long size = lx_dir(d->path,index,name);
        if (size == LX_END) break;
        if (size < LX_DIR) { d->failed = 1; break; }
        unsigned modified = lx_file_time(1);
        if (!strcmp(d->path,"/Trash") && !strcmp(name,".origins")) continue;
        if (d->filter[0] && !name_matches(name,d->filter)) continue;
        if (d->count == d->capacity) {
            int capacity = d->capacity ? 2*d->capacity : 32;
            Entry *rows = realloc(d->rows,capacity*sizeof(Entry));
            if (!rows) { d->failed = 1; break; }
            d->rows = rows;
            d->capacity = capacity;
        }
        char *copy = malloc(strlen(name)+1);
        if (!copy) { d->failed = 1; break; }
        strcpy(copy,name);
        d->rows[d->count++] = (Entry){copy,size,modified};
    }
    if (d->failed) clear_files(d);
    sort_files(d);
    scroll_files(d,d->first);
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
    int text = name_hit(d, d->mx, d->my);
    int left = d->mx - (text ? 4 : 0), top = d->my - (text ? 7 : 0);
    int w = min(CUR_W, d->w - left), h = min(CUR_H, d->h - top);
    uint32_t *tile = d->pixels + d->w * d->h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t c = d->pixels[(top + y) * d->w + left + x];
            char ink = text ? lixin_text_cursor[y][x] : lixin_cursor[y][x];
            tile[y * w + x] = draw ? lixin_cursor_pixel(ink, c) : c;
        }
    present(tile, left, top, w, h);
}
// Shared chrome. The popup host copies this same title strip for EL0 text windows.
static void titlebar(Desktop *d, int x, int y, int w, const char *title, int minimize) {
    box(d, x, y, w, TITLE, BAR);
    label(d, x + 14, y + 11, w - 100, title, INK);
    int right = x + w;
    box(d, right - 35, y + 8, 28, 28, PAPER);
    for (int i = 0; i < 10; ++i) {
        box(d, right - 26 + i, y + 17 + i, 2, 2, INK);
        box(d, right - 17 - i, y + 17 + i, 2, 2, INK);
    }
    if (minimize) {
        box(d, right - 68, y + 8, 28, 28, PAPER);
        box(d, right - 60, y + 25, 12, 2, INK);
    }
}
static int close_hit(int x, int y, int width) {
    return x >= width - 35 && x < width - 7 && y >= 8 && y < 36;
}
static int clamp(int value, int high) { return value < 0 ? 0 : min(value, high); }
static void disk(Desktop *d, int x, int y, int r, unsigned color) {
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r)
                box(d, x + i, y + j, 1, 1, color);
}
static void roundbox(Desktop *d, int x, int y, int w, int h, int r, unsigned color) {
    box(d, x + r, y, w - 2 * r, h, color);
    box(d, x, y + r, w, h - 2 * r, color);
    disk(d, x + r, y + r, r, color);
    disk(d, x + w - r - 1, y + r, r, color);
    disk(d, x + r, y + h - r - 1, r, color);
    disk(d, x + w - r - 1, y + h - r - 1, r, color);
}
enum { MENU_CLOSED, MENU_APPS, MENU_WINDOWS };
static int window_app_name(const char *name) {
    size_t n = strlen(name);
    return (n > 4 && !strcmp(name + n - 4, ".win")) ||
           (n > 5 && (!strcmp(name + n - 5, ".win2") || !strcmp(name + n - 5, ".win3")));
}
static const char *window_name(Desktop *d, int id) {
    static const char *const builtins[] = {"Files", "Browser", "Calculator"};
    if (id < FIRST_CLIENT) return builtins[id];
    const char *path = d->client_path[id - FIRST_CLIENT];
    const char *name = strrchr(path, '/') + 1;
    const AppInfo *info = app_info(name);
    return info ? info->title : name;
}
static int list_app(Desktop *d, const char *name, const AppInfo *info, int dock_id) {
    if (d->app_count == d->app_capacity) {
        int capacity = d->app_capacity ? d->app_capacity * 2 : 32;
        LauncherApp *apps = realloc(d->apps, capacity * sizeof *apps);
        if (!apps) return 0;
        d->apps = apps;
        d->app_capacity = capacity;
    }
    LauncherApp *app = &d->apps[d->app_count++];
    strcpy(app->name, name);
    app->info = info;
    app->dock_id = dock_id;
    ++d->app_totals[GROUP_ALL];
    ++d->app_totals[info ? info->group : GROUP_OTHER];
    return 1;
}
static void scan_apps(Desktop *d) {
    d->app_count = 0;
    memset(d->app_totals, 0, sizeof d->app_totals);
    for (int id = 0; id < EYES_ICON; ++id)
        if (!list_app(d, "", &dock_apps[id], id)) goto full;
    if (!list_app(d,"agent.win3",app_info("agent.win3"),-1)) goto full;
    for (unsigned i = 0;; ++i) {
        char name[513];
        long size = lx_dir("/bin", i, name);
        if (size == LX_END || size < LX_DIR) break;
        if (size == LX_DIR || !strcmp(name,"agent.win3")
            || (!window_app_name(name) && strcmp(name, "nxeyes")) || strlen(name) > 507)
            continue;
        if (!list_app(d,name,app_info(name),-1)) goto full;
    }
    if (!d->app_totals[d->app_group]) d->app_group = GROUP_ALL;
    return;
full:
    strcpy(d->message,"内存不足，应用列表未完整加载");
}
static void show_menu(Desktop *d, int menu) {
    d->menu = menu;
    if (menu == MENU_APPS) {
        d->app_scroll = 0;
        d->app_dragged = 1;
        scan_apps(d);
    }
}
static int eye_window(Desktop *d) {
    for (int i = APPS - 1; i >= 0; --i) {
        int id = d->order[i];
        if (id >= FIRST_CLIENT && d->windows[id].open &&
            !strcmp(d->client_path[id - FIRST_CLIENT], "/bin/nxeyes")) return id;
    }
    return -1;
}
static int step(Desktop *d) { return min(82, (d->h - 48) / DOCK_ITEMS); }
static int focus(Desktop *d);
static void dock(Desktop *d) {
    static const char *const names[] = {"Files", "Browser", "Calc", "Terminal", "Nano", "Editor", "Eyes", "Apps"};
    static const unsigned colors[] = {0xd78938, 0x3275ab, 0x447e70, 0x44374d, 0x8e5064, 0x576aa1, 0x70568c, 0x37776d};
    box(d, 0, 0, DOCK, d->h, 0x281e2b);
    box(d, DOCK - 1, 0, 1, d->h, 0x5b4050);
    for (int i = 0; i < DOCK_ITEMS; i++) {
        int id = i == EYES_ICON ? eye_window(d) : i < FIRST_CLIENT ? i : -1;
        int y = 8 + i * step(d), x = 18;
        if (id >= 0 && d->windows[id].open) {
            box(d, 2, y + 12, 3, 18, 0xf39a62);
            if (d->windows[id].shown && focus(d) == id)
                roundbox(d, 7, y - 2, 62, step(d) - 3, 8, 0x50404e);
        }
        roundbox(d, x, y + 2, 40, 38, 8, colors[i]);
        if (i == 0)
            folder(d, x + 2, y + 8, 2);
        else if (i == 1) {
            disk(d, x + 20, y + 21, 13, 0xcbf0f1);
            disk(d, x + 20, y + 21, 10, colors[i]);
            box(d, x + 8, y + 20, 25, 2, 0xcbf0f1);
            box(d, x + 19, y + 9, 2, 25, 0xcbf0f1);
        } else if (i == 2) {
            box(d, x + 9, y + 9, 22, 7, 0xeaf4e8);
            for (int k = 0; k < 6; k++)
                box(d, x + 9 + (k % 3) * 8, y + 20 + (k / 3) * 7, 5, 4, 0xeaf4e8);
        } else if (i == 3)
            label(d, x + 8, y + 10, 26, ">_", 0xf3e8eb);
        else if (i == EYES_ICON) {
            for (int dx = 12; dx <= 28; dx += 16) {
                disk(d, x + dx, y + 21, 7, PAPER);
                disk(d, x + dx + 2, y + 21, 3, INK);
            }
        } else if (i == APPS_ICON) {
            for (int j = 0; j < 4; ++j)
                box(d, x + 9 + 12 * (j % 2), y + 10 + 12 * (j / 2), 9, 9, PAPER);
        } else {
            box(d, x + 11, y + 8, 18, 24, PAPER);
            for (int k = 0; k < 3; k++)
                box(d, x + 14, y + 14 + 5 * k, 12 - 2 * k, 1, colors[i]);
        }
        label(d, (DOCK - label_width(names[i])) / 2, y + min(41, step(d) - 21), DOCK, names[i], 0xf7f0f2);
    }
    if (d->terminals) {
        char id[24];
        number(id, d->selected);
        int y = d->h - 40;
        box(d, 4, y, DOCK - 8, 32, 0x51404e);
        label(d, 9, y + 5, 12, "<", GOLD);
        label(d, 32, y + 5, 19, id, PAPER);
        label(d, 57, y + 5, 12, ">", GOLD);
    }
}
static void controls(Desktop *d, Bounds r) {
    int x = r.x + r.w - 99, y = r.y + 8;
    for (int i = 0; i < 3; i++)
        roundbox(d, x + i * 32, y, 28, 28, 6, i == 2 ? 0xf1d6cc : PAPER);
    box(d, x + 8, y + 17, 12, 2, INK);
    box(d, x + 40, y + 8, 12, 12, INK);
    box(d, x + 42, y + 10, 8, 8, PAPER);
    for (int i = 0; i < 10; i++) {
        box(d, x + 73 + i, y + 9 + i, 2, 2, INK);
        box(d, x + 82 - i, y + 9 + i, 2, 2, INK);
    }
}
static int sidebar(Bounds r) { return r.w >= 540 ? 112 : 0; }
static const char *const places[] = {"/", "/src", "/bin", "/Trash"};
static const char *const place_names[] = {"根目录", "源代码", "应用程序", "废纸篓"};
static const char *extension(const char *name) {
    const char *dot=strrchr(name,'.');
    return dot && dot!=name && dot[1] ? dot+1 : NULL;
}
static void select_row(Desktop *d,int index,int mods) {
    if ((mods&1) && d->range_anchor>=0) {
        int a=d->range_anchor;
        int first=min(a,index), last=a>index?a:index;
        for (int i=0;i<d->count;++i) d->rows[i].chosen=i>=first && i<=last;
    } else {
        if (!(mods&2)) for (int i=0;i<d->count;++i) d->rows[i].chosen=0;
        if (index>=0) d->rows[index].chosen=(mods&2) ? !d->rows[index].chosen : 1;
        d->range_anchor=index;
    }
    d->chosen=0; d->marked=-1;
    for (int i=0;i<d->count;++i) if (d->rows[i].chosen) { ++d->chosen; d->marked=i; }
    if (index>=0 && d->rows[index].chosen) d->marked=index;
}
static int action_enabled(Desktop *d, int a) {
    switch (actions[a].requirement) {
    case SINGLE: return d->chosen==1;
    case MANY: return d->chosen>0;
    case REGULAR: case EXTENSION:
        return d->chosen==1 && d->rows[d->marked].size>=0 &&
            (actions[a].requirement!=EXTENSION || extension(d->rows[d->marked].name));
    case CLIP: return d->clipboard_count>0;
    case PREVIOUS: return d->history_at>0;
    case NEXT: return d->history_at+1<d->history_count;
    case TRASH_ITEMS: return d->chosen && !strcmp(d->path,"/Trash");
    case UNDOABLE: return d->undo.count>0;
    case LIST: return d->count>0;
    default: return 1;
    }
}
static int place_count(Desktop *d) { return 4+(d->places->count ? 1+d->places->count : 0); }
static int place_rows(Bounds r) { return (r.h-FILE_FOOTER-80-(r.h>=300?36:0))/ROW; }
static const char *pin_label(Desktop *d) {
    return ui_place_index(d->places,d->path)>=0 ? "取消收藏" : "收藏目录";
}
static void draw_places(Desktop *d,Bounds r,int width) {
    int bottom=r.y+r.h-FILE_FOOTER;
    box(d,r.x,r.y+TITLE,width,bottom-r.y-TITLE,0xecf1eb);
    label(d,r.x+8,r.y+51,40,"位置",MUTED);
    label(d,r.x+50,r.y+51,width-51,ui_place_index(d->places,d->path)>=0?"- 收藏":"+ 收藏",INK);
    int rows=place_rows(r),count=place_count(d);
    d->places->first=ui_list_top(d->places->first,count,rows);
    for (int i=0;i<rows && i+d->places->first<count;++i) {
        int index=i+d->places->first, y=r.y+80+i*ROW;
        if (index==4) { label(d,r.x+9,y+3,width-16,"收藏夹",MUTED); continue; }
        const char *path=index<4 ? places[index] : d->places->path[index-5];
        if (!strcmp(d->path,path)) roundbox(d,r.x+4,y,width-8,28,5,0xd4e4d9);
        label(d,r.x+14,y+3,width-(index<4?20:36),index<4?place_names[index]:ui_place_label(path),INK);
        if (index>=5) label(d,r.x+width-20,y+3,16,"×",MUTED);
    }
    if (r.h>=300) {
        label(d,r.x+9,bottom-31,44,"后退",action_enabled(d,F_BACK)?INK:LINE);
        label(d,r.x+61,bottom-31,44,"前进",action_enabled(d,F_FORWARD)?INK:LINE);
    }
    box(d,r.x+width-1,r.y+TITLE,1,bottom-r.y-TITLE,LINE);
}
static int file_crumbs(Desktop *d,Bounds r,int hit) {
    UiCrumb crumbs[UI_CRUMBS];
    int n=ui_crumbs(d->path,r.w-217,label_width,crumbs);
    for (int i=0;i<n;++i) {
        UiCrumb b=crumbs[i];
        if (hit>=0) {
            if (hit>=b.x && hit<b.x+b.width) return b.end;
        } else {
            char name[513]; ui_crumb_label(d->path,b,name);
            int x=r.x+40+b.x;
            roundbox(d,x,r.y+8,b.width-2,28,4,BAR);
            label(d,x+4,r.y+11,b.width-8,name,INK);
        }
    }
    return 0;
}
static Bounds action_rect(Bounds r, int a) {
    int w=(r.w-24)/3, h=min(30,(r.h-50-FILE_FOOTER)/((F_COUNT+2)/3));
    return (Bounds){r.x+12+a%3*w,r.y+50+a/3*h,w-4,h-2};
}
/* Column positions are shared by painting and header hit testing. */
static void file_columns(int width, int x[FILE_COLUMNS+1]) {
    x[FILE_COLUMNS] = width-12;
    x[BY_DATE] = x[FILE_COLUMNS] - (width >= 640 ? 159 : 97);
    x[BY_SIZE] = x[BY_DATE] - (width >= 400 ? 83 : 65);
    x[BY_TYPE] = x[BY_SIZE] - (width >= 400 ? 72 : 53);
    x[BY_NAME] = 0;
}
static void format_size(char out[32], long bytes) {
    static const char *const units[] = {" B"," KiB"," MiB"," GiB"," TiB"};
    if (bytes < 0) { strcpy(out,"-"); return; }
    unsigned long value = bytes, scale = 1;
    int unit = 0;
    while (unit < 4 && value/scale >= 1024) { scale *= 1024; ++unit; }
    number(out,value/scale);
    if (unit && value/scale < 10 && value%scale) {
        char *end = out+strlen(out);
        *end++ = '.';
        number(end,(value%scale)*10/scale);
    }
    strcat(out,units[unit]);
}
static Bounds file_thumb(Desktop *d, Bounds r) {
    int height = r.h - 80 - FILE_FOOTER;
    int visible = file_rows(d);
    UiThumb thumb=ui_list_thumb(d->first,d->count,visible,height);
    return (Bounds){r.x+r.w-8,r.y+80+thumb.start,5,thumb.length};
}
static void draw_files(Desktop *d, Bounds r) {
    int l = r.x, t = r.y, w = r.w, bottom = t + r.h - 41;
    box(d, l, t, w, r.h, PAPER);
    titlebar(d, l, t, w, "", 0);
    folder(d, l + 14, t + 13, 1);
    file_crumbs(d,r,-1);
    box(d, l + w - 172, t + 8, 64, 28, PAPER);
    label(d, l + w - 167, t + 11, 54, "上一级", strcmp(d->path, "/") ? INK : MUTED);
    if (d->action_menu) {
        for (int a = 0; a < F_COUNT; ++a) {
            Bounds b = action_rect(r, a);
            roundbox(d, b.x, b.y, b.w, b.h, 4, BAR);
            label(d, b.x + 5, b.y + 3, b.w - 10, a==F_PIN ? pin_label(d) : actions[a].label, action_enabled(d,a) ? INK : 0x98a7a0);
        }
        const char *ext=d->chosen==1 ? extension(d->rows[d->marked].name) : NULL;
        label(d,l+16,bottom+9,w-32,"返回列表",INK);
        if (ext) {
            label(d,l+125,bottom+9,95,"文件类型：",MUTED);
            label(d,l+217,bottom+9,w-233,ext,MUTED);
        }
        return;
    }
    int side = sidebar(r);
    if (side || d->places_open) draw_places(d,r,side?side:r.w);
    if (!side && d->places_open) {
        label(d,l+12,bottom+9,w-24,"返回列表",INK); return;
    }
    l += side;
    w -= side;
    int col[FILE_COLUMNS+1];
    file_columns(w,col);
    static const char *const headers[] = {"名称","类型","大小","修改日期"};
    for (int c = 0; c < FILE_COLUMNS; ++c) {
        int x = l+col[c]+(c == BY_NAME ? 15 : 3);
        const char *title = headers[c];
        label(d,x,t+51,col[c+1]-col[c]-8,title,d->sort_column == c ? INK : MUTED);
        if (d->sort_column == c) {
            int tip = x+label_width(title)+3;
            for (int n = 0; n < 4; ++n)
                box(d,tip+3-n,t+58+(d->descending ? 3-n : n),2*n+1,1,INK);
        }
    }
    box(d, l, t + 77, w, 1, LINE);
    int end = min(d->count,d->first + file_rows(d));
    for (int i = d->first; i < end; ++i) {
        int y = t + 80 + (i-d->first) * ROW;
        Entry *e = &d->rows[i];
        if (e->chosen) roundbox(d,l+3,y,w-15,ROW-1,4,0xcde5df);
        if (e->size == LX_DIR) folder(d,l+15,y+8,1);
        else {
            box(d,l+17,y+6,12,16,0x94adaf);
            box(d,l+19,y+9,8,1,PAPER);
        }
        label(d,l+42,y+3,col[BY_TYPE]-46,e->name,INK);
        label(d,l+col[BY_TYPE]+3,y+3,col[BY_SIZE]-col[BY_TYPE]-6,file_kind(e),MUTED);
        char size[32], date[24] = "-";
        format_size(size,e->size);
        if (e->modified) {
            format_utc(date,e->modified);
            date[w >= 640 ? 16 : 10] = 0;
        }
        label(d,l+col[BY_SIZE]+3,y+3,col[BY_DATE]-col[BY_SIZE]-6,size,INK);
        label(d,l+col[BY_DATE]+3,y+3,col[FILE_COLUMNS]-col[BY_DATE]-5,date,MUTED);
    }
    if (d->count > file_rows(d)) {
        Bounds thumb = file_thumb(d,r);
        box(d,l+w-9,t+80,7,bottom-t-80,BAR);
        box(d,thumb.x,thumb.y,thumb.w,thumb.h,0x829a90);
    }
    if (d->failed)
        label(d, l + 16, t + 90, w - 32, "Directory unavailable", MUTED);
    else if (!d->count)
        label(d, l + 16, t + 90, w - 32, d->filter[0] ? "没有匹配项目" : "空目录", MUTED);
    l = r.x;
    w = r.w;
    box(d, l, bottom, w, 1, LINE);
    roundbox(d, l+8, bottom+6, 64, 28, 5, 0xd4e4d9);
    label(d, l+20, bottom+9, 48, "操作", INK);
    label(d, l+82, bottom+9, 54, "筛选", INK);
    label(d, l+139, bottom+9, w-278, d->filter, MUTED);
    char total[96];
    if (d->chosen) {
        unsigned long bytes=0;
        for (int i=0;i<d->count;++i) if (d->rows[i].chosen && d->rows[i].size>=0) bytes+=d->rows[i].size;
        strcpy(total,"已选 "); number(total+strlen(total),d->chosen); strcat(total," 项，共 ");
        format_size(total+strlen(total),bytes);
    } else { number(total,d->count); strcat(total," 项"); }
    int room=w-148,width=min(room,label_width(total));
    label(d,l+w-12-width,bottom+9,width,total,MUTED);
}
static Bounds button_rect(Bounds r, int i) {
    int w = (r.w - 28) / 4, h = (r.h - 108) / 5;
    return (Bounds){r.x + 14 + i % 4 * w, r.y + 98 + i / 4 * h, w - 5, h - 5};
}
static void draw_calc(Desktop *d, Bounds r) {
    box(d, r.x, r.y, r.w, r.h, 0xf3f1ee);
    titlebar(d, r.x, r.y, r.w, "Calculator", 0);
    roundbox(d, r.x + 14, r.y + 48, r.w - 28, 42, 6, 0x293c3b);
    label(d, r.x + 25, r.y + 59, r.w - 55,
          error == 1 ? "Cannot divide by zero"
          : error    ? "Result too large"
                     : input,
          0xf3f7ec);
    for (int i = 0; i < COUNT; i++) {
        Bounds b = button_rect(r, i);
        roundbox(d, b.x, b.y, b.w, b.h, 5, i == 18 ? 0xb85d38 : i % 4 == 3 ? 0x57665f : 0x354849);
        label(d, b.x + (b.w - label_width(labels[i])) / 2, b.y + (b.h - 22) / 2, b.w, labels[i], PAPER);
    }
}
static Bounds menu_rect(Desktop *d) {
    return (Bounds){DOCK + 4, 4, min(340, d->w - DOCK - 8), 84 + APPS * 32};
}

enum { GRID_TOP = 108, GRID_ROW = 136, GRID_PAD = 24 };
static int grid_columns(Desktop *d) {
    int columns = (d->w - 2 * GRID_PAD) / 145;
    return columns > 0 ? min(columns, 6) : 1;
}
static Bounds grid_cell(Desktop *d, int index) {
    int columns = grid_columns(d), width = (d->w - 2 * GRID_PAD) / columns;
    return (Bounds){GRID_PAD + index % columns * width,
                    GRID_TOP + index / columns * GRID_ROW - d->app_scroll, width, GRID_ROW};
}
static int grid_limit(Desktop *d) {
    int cols = grid_columns(d), rows = (d->app_totals[d->app_group] + cols - 1) / cols;
    int limit = GRID_TOP + rows * GRID_ROW + 12 - d->h;
    return limit > 0 ? limit : 0;
}
static int grid_scroll(Desktop *d, int offset) {
    int next = clamp(offset, grid_limit(d));
    if (next == d->app_scroll) return 0;
    d->app_scroll = next;
    return 1;
}
static int grid_visible(Desktop *d, LauncherApp *app) {
    return d->app_group == GROUP_ALL || d->app_group == (app->info ? app->info->group : GROUP_OTHER);
}
static Bounds group_button(Desktop *d, int group) {
    int count = 0, index = 0;
    for (int i = 0; i < GROUP_COUNT; ++i) if (d->app_totals[i]) {
        if (i < group) ++index;
        ++count;
    }
    int width = min(66,(d->w-2*GRID_PAD)/count);
    return (Bounds){GRID_PAD+width*index,62,width-4,30};
}
static void grid_icon(Desktop *d, LauncherApp *app, Bounds cell) {
    const AppInfo *info = app->info;
    int icon = info ? info->icon : ICON_WIDGETS;
    unsigned color = launcher_icons[icon].color;
    int x = cell.x + cell.w/2 - 37, y = cell.y + 10;
    roundbox(d,x,y+3,74,74,17,0x183344);
    roundbox(d,x,y,74,74,17,color);
    for (int row = 0; row < 16; ++row) {
        unsigned bits = launcher_icons[icon].rows[row];
        for (int col = 0; col < 16; ++col)
            if (bits & (1u << col)) box(d,x+13+3*col,y+13+3*row,3,3,0xfffcf2);
    }
    const char *title = info ? info->title : app->name;
    int width = min(cell.w-8,label_width(title));
    label(d,cell.x+(cell.w-width)/2,y+82,width,title,0xf4f8fa);
}
static void draw_launcher(Desktop *d) {
    for (int y = 0; y < d->h; y += 4)
        box(d,0,y,d->w,4,blend(0x203f51,0x294d5e,y*255/d->h));
    int shown = 0;
    for (int i = 0; i < d->app_count; ++i) {
        if (!grid_visible(d,&d->apps[i])) continue;
        Bounds cell = grid_cell(d,shown++);
        if (cell.y >= d->h) break;
        if (cell.y + GRID_ROW > GRID_TOP) grid_icon(d,&d->apps[i],cell);
    }
    int limit = grid_limit(d);
    if (limit) {
        int track = d->h-GRID_TOP-12, thumb = track*track/(track+limit);
        if (thumb < 18) thumb = 18;
        box(d,d->w-10,GRID_TOP+(track-thumb)*d->app_scroll/limit,3,thumb,0x87a6ae);
    }
    box(d,0,0,d->w,GRID_TOP,0x203f51);
    label(d,GRID_PAD,20,120,"应用程序",PAPER);
    roundbox(d,d->w-148,14,60,32,12,0x34576a);
    label(d,d->w-136,19,40,"窗口",PAPER);
    roundbox(d,d->w-78,14,60,32,12,0x426978);
    label(d,d->w-66,19,40,"完成",PAPER);
    for (int i = 0; i < GROUP_COUNT; ++i) if (d->app_totals[i]) {
        Bounds r = group_button(d,i);
        int selected = (int)d->app_group == i;
        if (selected) roundbox(d,r.x,r.y,r.w,r.h,12,0xc6e4dc);
        int width = min(label_width(group_labels[i]),r.w-4);
        label(d,r.x+(r.w-width)/2,r.y+4,width,group_labels[i],selected ? INK : 0xcadadd);
    }
}
static void draw_menu(Desktop *d) {
    Bounds r = menu_rect(d);
    box(d,r.x,r.y,r.w,r.h,PAPER);
    box(d,r.x,r.y,r.w/2,38,BAR);
    box(d,r.x+r.w/2,r.y,r.w/2,38,INK);
    label(d,r.x+12,r.y+8,r.w/2-18,"应用程序",INK);
    label(d,r.x+r.w/2+12,r.y+8,r.w/2-18,"窗口",PAPER);
    int row = 0;
    for (int i = 0; i < APPS; ++i) {
        if (!d->windows[i].open) continue;
        int y = r.y + 42 + 32 * row++;
        label(d,r.x+10,y+3,r.w-94,window_name(d,i),INK);
        if (i >= FIRST_CLIENT) {
            char slot[24];
            number(slot, i - FIRST_CLIENT + 1);
            label(d,r.x+r.w-84,y+3,18,slot,MUTED);
        }
        label(d,r.x+r.w-65,y+3,62,d->windows[i].shown ? "Open" : "Hidden",MUTED);
        box(d,r.x+8,y+30,r.w-16,1,LINE);
    }
    if (!row) label(d,r.x+12,r.y+49,r.w-24,"没有打开的窗口",MUTED);
}
static uint32_t rgb565(unsigned pixel) {
    return ((pixel & 0xf800) << 8) | ((pixel & 0x07e0) << 5) | ((pixel & 31) << 3);
}
static void draw_surface(Desktop *d, WindowClient *c, Bounds r) {
    if (!c->ready) { box(d, r.x, r.y + TITLE, r.w, r.h - TITLE, INK); return; }
    for (int y = 0; y < r.h - TITLE; ++y) {
        const unsigned short *src = c->pixels + (y * c->height / (r.h - TITLE)) * c->width;
        uint32_t *dst = d->pixels + (r.y + TITLE + y) * d->w + r.x;
        if (r.w == c->width) {
            for (int x = 0; x < r.w; ++x) dst[x] = rgb565(src[x]);
        } else {
            for (int x = 0; x < r.w; ++x) dst[x] = rgb565(src[x * c->width / r.w]);
        }
    }
}

/* Small visible damage can go straight to the framebuffer. Return zero when
 * the compositor must also account for chrome, overlays or a scaled preview. */
static int present_patch(Desktop *d, int id) {
    WindowClient *c = &d->clients[id - FIRST_CLIENT];
    Bounds r = d->windows[id].rect;
    WindowUpdate u = c->reply.frame;
    if (d->wallpaper_menu || d->menu || d->naming || d->message[0] || r.w != c->width || r.h - TITLE != c->height ||
        (u.x + u.w > r.w - 14 && u.y + u.h > c->height - 5)) return 0;
    int x = r.x + u.x, y = r.y + TITLE + u.y;
    for (int i = APPS - 1; d->order[i] != id; --i) {
        Window *other = &d->windows[d->order[i]];
        if (!other->shown) continue;
        Bounds b = other->rect;
        if (x >= b.x && y >= b.y && x + u.w <= b.x+b.w && y+u.h <= b.y+b.h) return 1;
        if (x < b.x+b.w+3 && y < b.y+b.h+3 && x+u.w > b.x && y+u.h > b.y) return 0;
    }
    for (int row = 0; row < u.h; ++row) {
        uint32_t *dst = d->pixels + (y+row)*d->w + x;
        const unsigned short *src = c->pixels + (u.y+row)*c->width + u.x;
        for (int col = 0; col < u.w; ++col) dst[col] = rgb565(src[col]);
        present(dst, x, y+row, u.w, 1);
    }
    if (x < d->mx+CUR_W && y < d->my+CUR_H && x+u.w > d->mx-4 && y+u.h > d->my-7) cursor(d, 1);
    return 1;
}
enum { WALL_WIDTH=200, WALL_ROW=30, WALL_COUNT=3, WALL_HEIGHT=(WALL_COUNT+2)*WALL_ROW };
static const char *const wall_names[]={"暮色","青山","海岸"};
static const uint32_t skies[]={0x38253e,0xb7d9ce,0x6cacc3};
static const uint32_t horizons[]={0x6a3747,0xf2e0bf,0xf3ddbd};
static const char wall_path[]="/home/lixin/.wallpaper";
#include "wallpaper.h"

static void restore_wallpaper(Desktop *d) {
    long size = lx_stat(wall_path);
    if (size < 1 || size > 512) return;
    int fd = lx_open(wall_path);
    if (fd < 0) return;
    char saved[513];
    int used = 0;
    while (used < size) {
        int n = lx_read(fd,saved + used,size - used);
        if (n <= 0) break;
        used += n;
    }
    lx_close(fd);
    saved[used] = 0;
    if (used != size || strlen(saved) != (size_t)used) return;
    if (used == 1 && saved[0] >= '0' && saved[0] < '0' + WALL_COUNT) {
        d->wallpaper = saved[0] - '0';
    } else if (saved[0] == '/') {
        d->wall_image = load_wall_image(saved,d->w - DOCK,d->h);
        if (d->wall_image) strcpy(d->wall_file,saved);
        else strcpy(d->message,"无法加载已保存的壁纸");
    }
}
static int set_image_wallpaper(Desktop *d) {
    char path[513];
    if (!ui_file_path(path,d->cwd,d->filename)) {
        strcpy(d->message,"图片路径为空或过长");
        return 0;
    }
    uint32_t *image = load_wall_image(path,d->w - DOCK,d->h);
    if (!image) {
        strcpy(d->message,"无法读取图片，请检查 PNG 或 JPG 文件及大小");
        return 0;
    }
    if (lx_save_file(wall_path,path,strlen(path)) < 0) {
        free(image);
        strcpy(d->message,"无法保存壁纸设置");
        return 0;
    }
    free(d->wall_image);
    d->wall_image = image;
    strcpy(d->wall_file,path);
    return 1;
}
static void paint_wallpaper(Desktop *d) {
    if (d->wall_image) {
        int width = d->w - DOCK;
        for (int y=0; y<d->h; ++y)
            memcpy(d->pixels + y*d->w + DOCK,d->wall_image + y*width,width*sizeof(uint32_t));
        return;
    }
    for (int y = 0; y < d->h; y += 8)
        box(d, DOCK, y, d->w - DOCK, min(8, d->h - y), blend(skies[d->wallpaper], horizons[d->wallpaper], y * 255 / d->h));
    if (d->wallpaper) {
        for (int x=DOCK;x<d->w;x+=2) {
            int width=min(2,d->w-x);
            int u=(x-DOCK)*1024/(d->w-DOCK);
            int ridge=abs((u+90)%510-255), near=abs((u+240)%780-390);
            int mountain=d->wallpaper==1;
            int y=d->h*(mountain?420+ridge:650+ridge/5)/1024;
            box(d,x,y,width,d->h-y,mountain?0x61877c:0x438ca0);
            y=d->h*(mountain?615+near/2:810+near/5)/1024;
            box(d,x,y,width,d->h-y,mountain?0x355f58:0x28677b);
        }
    }
}
static void render(Desktop *d) {
    if (d->menu == MENU_APPS) {
        draw_launcher(d);
        goto overlays;
    }
    paint_wallpaper(d);
    dock(d);
    for (int i = 0; i < APPS; i++) {
        int id = d->order[i];
        Window *win = &d->windows[id];
        if (!win->shown)
            continue;
        Bounds r = win->rect;
        if (r.x + r.w + 3 <= d->w && r.y + r.h + 3 <= d->h)
            box(d, r.x + 3, r.y + 3, r.w, r.h, 0x342133);
        if (id == FILE_WIN)
            draw_files(d, r);
        if (id == CALC_WIN)
            draw_calc(d, r);
#ifndef __TINYC__
        if (id == WEB_WIN) {
            desk_web_draw(r.x, r.y, r.w, r.h);
            titlebar(d, r.x, r.y, r.w, desk_web_title(), 0);
        }
#endif
        if (id >= FIRST_CLIENT) {
            WindowClient *client = &d->clients[id - FIRST_CLIENT];
            titlebar(d, r.x, r.y, r.w, window_name(d, id), 0);
            draw_surface(d, client, r);
        }
        controls(d, r);
        for (int n = 0; n < 3; n++)
            box(d, r.x + r.w - 5 - n * 3, r.y + r.h - 4, 2, 2, 0x87938e);
    }
    if (d->menu) draw_menu(d);
    if (d->wallpaper_menu) {
        int x=d->wallpaper_x,y=d->wallpaper_y;
        box(d,x,y,WALL_WIDTH,WALL_HEIGHT,PAPER);
        box(d,x,y,WALL_WIDTH,WALL_ROW,BAR);
        label(d,x+12,y+4,170,"切换壁纸",INK);
        for(int i=0;i<WALL_COUNT;i++) {
            int row=y+(i+1)*WALL_ROW;
            if(!d->wall_image && i==d->wallpaper) box(d,x,row,WALL_WIDTH,WALL_ROW,0xd8e9dc);
            box(d,x+12,row+7,24,16,skies[i]);
            label(d,x+48,row+4,140,wall_names[i],INK);
        }
        int row = y + (WALL_COUNT+1)*WALL_ROW;
        if (d->wall_image) box(d,x,row,WALL_WIDTH,WALL_ROW,0xd8e9dc);
        label(d,x+12,row+4,WALL_WIDTH-24,"指定图片 PNG / JPG",INK);
    }
    if (d->naming) {
        int x = d->dialog_x, y = d->dialog_y;
        box(d, x, y, 440, 120, PAPER);
        titlebar(d, x, y, 440, d->naming == EDITOR_NAME ? "文本编辑器" :
                 d->naming == WALL_PROMPT ? "壁纸图片路径 (PNG / JPG)" : actions[d->naming - FILE_PROMPT].label, 0);
        box(d, x + 12, y + 50, 416, 28, BAR);
        const char *tail = d->filename + d->name_offset;
        const char *caret = d->filename + d->name.caret;
        if (tail > caret) tail = caret;
        int caret_x;
        for (;;) {
            const char *p = tail;
            caret_x = 0;
            while (p < caret) caret_x += label_advance(utf8_next(&p), NULL);
            if (caret_x <= 390) break;
            utf8_next(&tail);
        }
        d->name_offset = tail - d->filename;
        int lo = min(d->name.anchor, d->name.caret);
        int hi = d->name.anchor + d->name.caret - lo;
        int left = 0;
        for (const char *p = tail; *p && left < 390;) {
            int index = p - d->filename;
            int advance = label_advance(utf8_next(&p), NULL);
            if (index >= lo && index < hi)
                box(d, x + 18 + left, y + 52, min(advance, 390 - left), 24, 0xb2d7f3);
            left += advance;
        }
        label(d, x + 18, y + 53, 390, tail, INK);
        box(d, x + 18 + caret_x, y + 54, 2, 20, 0x168178);
        if (d->naming == EDITOR_NAME)
            label(d,x+14,y+87,410,"Enter: open or create     Esc: cancel",MUTED);
        else {
            roundbox(d,x+12,y+84,88,28,4,0xd4e4d9);
            roundbox(d,x+104,y+84,88,28,4,BAR);
            label(d,x+35,y+87,54,"确定",INK);
            label(d,x+127,y+87,54,"取消",INK);
        }
    }
overlays:
    if (d->message[0]) {
        box(d, 8, d->h - 30, d->w - 16, 28, INK);
        label(d, 18, d->h - 27, d->w - 36, d->message, PAPER);
    }
    present(d->pixels, 0, 0, d->w, d->h);
    cursor(d, 1);
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
    strcpy((char *)(mail + 2), action < 0 ? d->filename : "");
#ifdef __TINYC__
    lx_agent_yield();
#else
#ifdef LX_NETSURF
    __asm__ volatile("mrs %0,tpidr_el0" : "=r"(desktop_thread_pointer));
#endif
    if (!setjmp(WAITING)) {
        console_call(32, 0);
        __builtin_unreachable();
    }
#endif
    d->terminals = mail[0];
    if (!action || d->selected > d->terminals)
        d->selected = d->terminals;
    unsigned n = 0;
    while (n + 1 < sizeof(d->message) && ((char *)(mail + 2))[n]) {
        d->message[n] = ((char *)(mail + 2))[n];
        ++n;
    }
    d->message[n] = 0;
    if (d->windows[FILE_WIN].open)
        load(d);
}
static int contains(Bounds r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
static int window_at(Desktop *d, int x, int y) {
    for (int i = APPS - 1; i >= 0; i--) {
        int id = d->order[i];
        if (d->windows[id].shown && contains(d->windows[id].rect, x, y))
            return id;
    }
    return -1;
}
static int focus(Desktop *d) {
    for (int i = APPS - 1; i >= 0; i--)
        if (d->windows[d->order[i]].shown)
            return d->order[i];
    return -1;
}
static void raise_window(Desktop *d, int id) {
    int i = 0;
    while (d->order[i] != id)
        i++;
    for (; i + 1 < APPS; i++)
        d->order[i] = d->order[i + 1];
    d->order[APPS - 1] = id;
}
static void close_window(Desktop *d, int id) {
    Window *win = &d->windows[id];
    win->open = win->shown = 0;
    if (id == FILE_WIN) {
        clear_files(d);
        free(d->rows);
        d->rows = NULL;
        d->capacity = 0;
    }
    if (id >= FIRST_CLIENT)
        client_close(&d->clients[id - FIRST_CLIENT]);
    if (d->client_grab == id) d->client_grab = -1;
#ifndef __TINYC__
    if (id == WEB_WIN)
        desk_web_close();
#endif
}
static void configure_window(Desktop *d, int id) {
    if (id < FIRST_CLIENT) return;
    WindowClient *c = &d->clients[id - FIRST_CLIENT];
    Bounds r = d->windows[id].rect;
    if (c->adaptive) client_event(c, (WindowEvent){WINDOW_CLIENT_CONFIGURE, r.w, r.h-TITLE,
        c->extended ? WINDOW_CLIENT_RIGHT_CLICK_AVAILABLE : 0});
}
static int install_agent_window(void) {
    static const char arguments[] = "/\0/bin/tcc\0tcc\0/src/desktop/agent-panel.c\0-o\0/bin/agent.win3";
    memcpy((void *)0x443d8000,arguments,sizeof arguments);
    long pid = console_call(50, ((long)sizeof arguments << 32) | 0x020100L);
    return pid > 0 && console_call(51,pid) == 0;
}

static void open_client(Desktop *d, const char *path, const char *argument) {
    for (int i = 0; i < CLIENT_LIMIT; ++i) {
        WindowClient *client = &d->clients[i];
        if (client->pid) continue;
        if (!strcmp(path,"/bin/agent.win3") && lx_stat(path) == LX_MISSING && !install_agent_window()) {
            strcpy(d->message,"Could not compile the Agent window");
            return;
        }
        if (!client_open_argument(client, d->cwd, path, argument)) {
            strcpy(d->message, "Could not start window application");
            return;
        }
        int id = FIRST_CLIENT + i;
        int w = min(client->extended ? 720 : 360, d->w - DOCK - 12);
        int h = min(client->extended ? 480 : 246, d->h - 12);
        int x = DOCK + (d->w - DOCK - w) / 2, y = (d->h - h) / 2;
        d->windows[id] = (Window){.rect = {min(x + i*28, d->w-w), min(y + i*28, d->h-h), w, h},
                                  .open = 1, .shown = 1};
        strcpy(d->client_path[i], path);
        if (client->adaptive) configure_window(d, id);
        else client_event(client, (WindowEvent){WINDOW_CLIENT_POINTER, WINDOW_CLIENT_WIDTH/2, WINDOW_CLIENT_HEIGHT/2, 0});
        raise_window(d, id);
        return;
    }
    strcpy(d->message, "All four application windows are in use");
}
static int click(Desktop *d, int x, int y);
static void menu_click(Desktop *d, int x, int y) {
    if (d->menu == MENU_APPS) {
        if (y >= 14 && y < 46 && x >= d->w-148 && x < d->w-88)
            show_menu(d,MENU_WINDOWS);
        else if (y >= 14 && y < 46 && x >= d->w-78 && x < d->w-18)
            d->menu = MENU_CLOSED;
        else if (y < GRID_TOP) {
            for (int i = 0; i < GROUP_COUNT; ++i) {
                if (d->app_totals[i] && contains(group_button(d,i),x,y)) {
                    d->app_group = i;
                    d->app_scroll = 0;
                    break;
                }
            }
        } else {
            int index = 0;
            for (int i = 0; i < d->app_count; ++i) {
                LauncherApp *app = &d->apps[i];
                if (!grid_visible(d,app)) continue;
                Bounds cell = grid_cell(d,index++);
                Bounds tile = {cell.x+4,cell.y+8,cell.w-8,106};
                if (!contains(tile,x,y)) continue;
                d->menu = MENU_CLOSED;
                if (app->dock_id >= 0) click(d,DOCK/2,8+app->dock_id*step(d)+20);
                else {
                    FilePath path = "/bin/";
                    strcat(path,app->name);
                    open_client(d,path,NULL);
                }
                break;
            }
        }
        return;
    }
    Bounds r = menu_rect(d);
    if (contains(r, x, y)) {
        x -= r.x; y -= r.y;
        if (y < 38) {
            show_menu(d, x < r.w/2 ? MENU_APPS : MENU_WINDOWS);
            return;
        }
        if (y < 42 || y >= 42 + APPS * 32) return;
        int row = (y - 42) / 32;
        int id = 0;
        for (; id < APPS; ++id) if (d->windows[id].open && row-- == 0) break;
        if (id == APPS) return;
        d->windows[id].shown = 1;
        raise_window(d, id);
    }
    d->menu = MENU_CLOSED;
}
/* File mutations use the same transactional ABI as cp/mv/mkdir. */
enum { FS_BEGIN, FS_MKDIR, FS_TOUCH, FS_UNLINK, FS_COPY, FS_MOVE, FS_COMMIT, FS_ABORT, FS_EXISTS, FS_PATH_RECORD };
static long fs_request(Desktop *d, int op, const char *path, const char *other) {
    char *mail = (char *)0x443d7000UL;
    if (path) strcpy(mail,path);
    if (other) strcpy(mail+513,other);
    long result = console_call(45,op);
    if (result < 0) {
        size_t n = 0;
        while (n+1 < sizeof d->message && mail[n]) { d->message[n] = mail[n]; ++n; }
        d->message[n] = 0;
    }
    return result;
}
static int child_path(Desktop *d, char out[513], const char *parent, const char *name) {
    if (!*name || strchr(name,'/') || !strcmp(name,".") || !strcmp(name,"..")) {
        strcpy(d->message,"请输入单个文件名"); return 0;
    }
    if (strlen(parent) + (parent[1] != 0) + strlen(name) > 512) {
        strcpy(d->message,"路径过长"); return 0;
    }
    strcpy(out,parent);
    if (parent[1]) strcat(out,"/");
    strcat(out,name);
    return 1;
}
static int navigate(Desktop *d,const char *path,int dest) {
    if (lx_stat(path)!=LX_DIR) { strcpy(d->message,"目录不存在或已移动"); return 0; }
    d->places_open=d->action_menu=0;
    if (dest<0 && !strcmp(path,d->path)) return 1;
    FileVisit *leaving=&d->history[d->history_at];
    FileView *snapshot=file_view(d->filter,d->rows,d->count,d->chosen,d->first,d->marked,
                                 d->range_anchor,d->sort_column,d->descending);
    if (!snapshot) { strcpy(d->message,"内存不足"); return 0; }
    free(leaving->view); leaving->view=snapshot;
    if (dest<0) {
        for (int i=d->history_at+1;i<d->history_count;++i) free(d->history[i].view);
        if (d->history_at+1==HISTORY) {
            free(d->history[0].view);
            memmove(d->history,d->history+1,(HISTORY-1)*sizeof(FileVisit));
            --d->history_at;
        }
        dest=d->history_at+1; d->history_count=dest+1;
        strcpy(d->history[dest].path,path); d->history[dest].view=NULL;
    }
    d->history_at=dest;
    FileView *v=d->history[dest].view;
    strcpy(d->path,d->history[dest].path);
    strcpy(d->filter,v?v->filter:"");
    d->first=v?v->first:0;
    if (v) { d->sort_column=v->column; d->descending=v->descending; }
    load(d);
    if (v) {
        for (int i=0;i<d->count;++i) {
            Entry *row=&d->rows[i];
            if (view_chosen(v,row->name)) { row->chosen=1; ++d->chosen; if (d->marked<0) d->marked=i; }
            if (!strcmp(row->name,v->focus)) d->marked=i;
            if (!strcmp(row->name,v->anchor)) d->range_anchor=i;
            if (!strcmp(row->name,v->top)) d->first=i;
        }
        scroll_files(d,d->first);
    }
    return 1;
}
static void update_place(Desktop *d,const char *directory,int remove_only) {
    FilePath path; strcpy(path,directory);
    if (fs_request(d,FS_BEGIN,NULL,NULL)<0) return;
    UiPlaces *p=d->places;
    ui_places_read(p);
    int index=ui_place_index(p,path);
    char record[64];
    if (index>=0) {
        ui_place_record(p->slot[index],record);
        if (fs_request(d,FS_UNLINK,record,NULL)<0) return;
    } else {
        if (remove_only) { fs_request(d,FS_ABORT,NULL,NULL); return; }
        if (fs_request(d,FS_MKDIR|256,"/home",NULL)<0 ||
            fs_request(d,FS_MKDIR|256,"/home/lixin",NULL)<0 ||
            fs_request(d,FS_MKDIR|256,UI_PLACES_DIR,NULL)<0) return;
        int slot;
        for (slot=0;slot<UI_FAVORITES;++slot) {
            ui_place_record(slot,record);
            long exists=fs_request(d,FS_EXISTS,record,NULL);
            if (exists<0) return;
            if (!exists) break;
        }
        if (slot==UI_FAVORITES) {
            fs_request(d,FS_ABORT,NULL,NULL); strcpy(d->message,"收藏夹已满"); return;
        }
        if (fs_request(d,FS_PATH_RECORD,record,path)<0) return;
    }
    if (fs_request(d,FS_COMMIT,NULL,NULL)<0) return;
    ui_places_read(p);
    int added=ui_place_index(p,path);
    if (added>=0) p->first=ui_list_reveal(p->first,5+added,place_count(d),place_rows(d->windows[FILE_WIN].rect));
}
static int copy_tree(Desktop *d, char source[513], char target[513]) {
    struct Level { unsigned src, dst, next; } levels[257];
    int depth = 1;
    levels[0] = (struct Level){strlen(source),strlen(target),0};
    if (fs_request(d,FS_MKDIR,target,NULL) < 0) return 0;
    while (depth) {
        struct Level *level = &levels[depth-1];
        source[level->src] = target[level->dst] = 0;
        char name[513];
        long kind = lx_dir(source,level->next++,name);
        if (kind == LX_END) { --depth; continue; }
        if (kind < LX_DIR) { strcpy(d->message,"读取目录失败"); return 0; }
        size_t len = strlen(name);
        if (level->src+1+len > 512 || level->dst+1+len > 512 || depth == 257) {
            strcpy(d->message,"路径过长"); return 0;
        }
        strcat(source,"/"); strcat(source,name);
        strcat(target,"/"); strcat(target,name);
        if (kind == LX_DIR) {
            if (fs_request(d,FS_MKDIR,target,NULL) < 0) return 0;
            levels[depth++] = (struct Level){strlen(source),strlen(target),0};
        } else if (fs_request(d,FS_COPY,source,target) < 0) return 0;
    }
    return 1;
}
#include "file-operations.h"
static int video_filename(const char *name) {
    const char *ext = strrchr(name,'.');
    if (!ext || strlen(ext) > 5) return 0;
    char lower[6];
    for (int i=0;;++i) {
        char c=ext[i];
        lower[i]=c>='A' && c<='Z' ? c+('a'-'A') : c;
        if (!c) break;
    }
    const char *types[]={".mp4",".m4v",".mov",".avi",".mkv",".webm",".mpg",".mpeg"};
    for (unsigned i=0;i<sizeof types/sizeof types[0];++i)
        if (!strcmp(lower,types[i])) return 1;
    return 0;
}
static void open_marked(Desktop *d) {
    Entry *e = &d->rows[d->marked];
    if (!child_path(d,d->filename,d->path,e->name)) return;
    if (e->size == LX_DIR) { navigate(d,d->filename,-1); return; }
    if (window_app_name(e->name)) {
        open_client(d, d->filename, NULL);
        return;
    }
    if (video_filename(e->name) && lx_stat("/bin/video.win3") >= 0) {
        open_client(d, "/bin/video.win3", d->filename);
        return;
    }
    int app = 0;
    size_t filename_length = strlen(e->name);
    if (e->size >= 16 && filename_length >= 4 && !strcmp(e->name + filename_length - 4, ".app")) {
        char magic[4];
        int fd = lx_open(d->filename);
        if (fd >= 0) {
            app = lx_read(fd, magic, sizeof(magic)) == sizeof(magic) &&
                  !memcmp(magic, "LXE1", sizeof(magic));
            lx_close(fd);
        }
    }
    launch(d, app ? -2 : preferred_editor(d) ? -3 : -1);
}
static void file_action(Desktop *d, int a) {
    if (!action_enabled(d,a)) return;
    d->action_menu = 0;
    d->message[0] = 0;
    if (a == F_OPEN) open_marked(d);
    else if (a==F_USE_EDITOR || a==F_USE_NANO || a==F_ALWAYS_EDITOR || a==F_ALWAYS_NANO) {
        int graphical=a==F_USE_EDITOR || a==F_ALWAYS_EDITOR;
        if ((a==F_ALWAYS_EDITOR || a==F_ALWAYS_NANO) && !set_editor(d,graphical)) return;
        if (child_path(d,d->filename,d->path,d->rows[d->marked].name)) launch(d,graphical?-3:-1);
    } else if (a==F_PIN) update_place(d,d->path,0);
    else if (a==F_PLACES) { d->places_open=1; d->places->first=0; }
    else if (a==F_ALL) {
        for (int i=0;i<d->count;++i) d->rows[i].chosen=1;
        d->chosen=d->count; d->marked=d->range_anchor=d->first; d->clicked_at=-1;
    } else if (a==F_COPY || a==F_CUT) capture_selection(d,a==F_CUT);
    else if (a==F_PASTE || a==F_TRASH || a==F_RESTORE || a==F_UNDO) apply_file_action(d,a,NULL);
    else if (a == F_SHOW_TRASH) {
        if (lx_stat("/Trash") == LX_MISSING) strcpy(d->message,"废纸篓为空");
        else navigate(d,"/Trash",-1);
    } else if (a == F_BACK || a == F_FORWARD) {
        int dest = d->history_at + (a == F_BACK ? -1 : 1);
        navigate(d,d->history[dest].path,dest);
    } else {
        d->naming = FILE_PROMPT + a;
        const char *value = a == F_RENAME ? d->rows[d->marked].name :
                            a == F_FIND ? d->filter : a == F_LOCATION ? d->path : "";
        strcpy(d->filename,value);
        name_begin(d, 1);
    }
}
static void submit_file_prompt(Desktop *d) {
    int a = d->naming - FILE_PROMPT;
    d->message[0] = 0;
    const char *scan = d->filename;
    while (*scan) {
        if (utf8_next(&scan) == 0xfffd) { strcpy(d->message,"文件名编码无效"); return; }
    }
    if (d->naming == WALL_PROMPT) {
        if (!set_image_wallpaper(d)) return;
    } else if (a == F_FIND) {
        strcpy(d->filter,d->filename);
        d->first = 0;
        load(d);
    } else if (a == F_LOCATION) {
        char absolute[513];
        if (!ui_file_path(absolute,d->path,d->filename)) {
            strcpy(d->message,"目录路径为空或过长"); return;
        }
        if (!navigate(d,absolute,-1)) return;
    } else if (a==F_UNDO || a==F_RESTORE) { if (!retry_destination(d)) return; }
    else if (!apply_file_action(d,a,d->filename)) return;
    d->naming = 0;
}
static int file_key(Desktop *d, int key) {
    if (key == 27) {
        int prefix = lx_timed_byte(20);
        if (prefix == '[' || prefix == 'O') {
            switch (lx_timed_byte(30)) {
            case 'A': key = 'k'; break;
            case 'B': key = 'j'; break;
            case 'D': file_action(d,F_BACK); return 1;
            case 'C': file_action(d,F_FORWARD); return 1;
            default: return 1;
            }
        }
    }
    int command=ui_command_for_key(actions,F_COUNT,key==10 ? 13 : key);
    if (command>=0) { file_action(d,command); return 1; }
    switch (key) {
    case 'j': case 'k':
        if (d->action_menu || !d->count) break;
        select_row(d,d->marked<0 ? d->first : clamp(d->marked+(key=='j'?1:-1),d->count-1),0);
        reveal_selection(d);
        d->clicked_at = -1;
        break;
    case 27:
        if (d->action_menu) d->action_menu = 0;
        else if (d->filter[0]) { d->filter[0] = 0; d->first = 0; load(d); }
        else return 0;
        break;
    default: return 0;
    }
    return 1;
}

static int click(Desktop *d, int x, int y) {
    if (d->naming) {
        int dx = x - d->dialog_x, dy = y - d->dialog_y;
        if (close_hit(dx, dy, 440)) {
            d->naming = 0;
            return 1;
        }
        if (d->naming >= FILE_PROMPT && dy >= 84 && dy < 115 && dx >= 12 && dx < 192) {
            if (dx < 103) submit_file_prompt(d);
            else d->naming = 0;
            return 1;
        }
        if (name_hit(d, x, y)) {
            d->name.caret = d->name.anchor = name_position(d, x);
            d->name_drag = 1;
            return 1;
        }
        if (dx >= 0 && dx < 440 && dy >= 0 && dy < TITLE) {
            d->dragging = 2;
            d->grab_x = dx;
            d->grab_y = dy;
        }
        return 0;
    }
    d->message[0] = 0;
    if (d->menu) {
        menu_click(d, x, y);
        return 1;
    }
    if (x < DOCK) {
        if (d->terminals && y >= d->h - 40 && y < d->h - 8) {
            if (x < 25)
                d->selected = d->selected > 1 ? d->selected - 1 : d->terminals;
            else if (x > 50)
                d->selected = d->selected % d->terminals + 1;
            else
                launch(d, d->selected);
            return 1;
        }
        if (y < 8)
            return 0;
        int id = (y - 8) / step(d);
        if (id == APPS_ICON) {
            show_menu(d, MENU_APPS);
        } else if (id == EYES_ICON) {
            int eye = eye_window(d);
            if (eye < 0) open_client(d, "/bin/nxeyes", NULL);
            else {
                d->windows[eye].shown = 1;
                raise_window(d, eye);
            }
        } else if (id < FIRST_CLIENT) {
#ifdef __TINYC__
            if (id == WEB_WIN) {
                strcpy(d->filename, "/bin/links");
                launch(d, -2);
                return 1;
            }
#endif
            Window *win = &d->windows[id];
            if (!win->open) {
                if (id == FILE_WIN) {
                    strcpy(d->path, "/");
                    for (int i=0;i<d->history_count;++i) free(d->history[i].view);
                    memset(d->history,0,sizeof d->history);
                    strcpy(d->history[0].path,"/");
                    ui_places_read(d->places); d->places->first=d->places_open=0;
                    d->history_at = d->action_menu = 0;
                    d->history_count = 1;
                    d->filter[0] = 0;
                    d->first = 0;
                    load(d);
                }
                if (id == CALC_WIN)
                    clear();
#ifndef __TINYC__
                if (id == WEB_WIN &&
                    !desk_web_open(d->pixels, d->w, d->h, d->cwd, win->rect.w, win->rect.h)) {
                    strcpy(d->message, "Could not open browser");
                    return 1;
                }
#endif
                win->open = 1;
            }
            win->shown = 1;
            raise_window(d, id);
        } else if (id == TERMINAL_ICON)
            launch(d, 0);
        else if (id == NANO_ICON) {
            strcpy(d->filename, d->windows[FILE_WIN].open ? d->path : "/");
            size_t n = strlen(d->filename);
            if (n < 512 && d->filename[n - 1] != '/')
                strcat(d->filename, "/");
            d->naming = EDITOR_NAME;
            name_begin(d, 0);
        } else if (id == EDITOR_ICON) {
            strcpy(d->filename, "/bin/text-editor");
            launch(d, -2);
        }
        return 1;
    }
    int id = window_at(d, x, y);
    if (id < 0)
        return 0;
    raise_window(d, id);
    Window *win = &d->windows[id];
    Bounds r = win->rect;
    int ax = x, ay = y;
    x -= r.x;
    y -= r.y;
    if (y >= 8 && y < 36 && x >= r.w - 99 && x < r.w - 7 && (x - r.w + 99) % 32 < 28) {
        int control = (x - r.w + 99) / 32;
        if (control == 0)
            win->shown = 0;
        else if (control == 1) {
            if (win->maximized)
                win->rect = win->restored;
            else {
                win->restored = r;
                win->rect = (Bounds){DOCK, 0, d->w - DOCK, d->h};
            }
            win->maximized = !win->maximized;
            configure_window(d, id);
            if (id == FILE_WIN) scroll_files(d,d->first);
        } else if (id >= FIRST_CLIENT && d->clients[id - FIRST_CLIENT].extended) {
            WindowClient *client = &d->clients[id - FIRST_CLIENT];
            long now = console_call(38, 0);
            if (client->confirm_force_close) {
                close_window(d, id);
            } else if (!client->close_requested_at) {
                client->close_requested_at = now;
                client_event(client, (WindowEvent){.type = WINDOW_CLIENT_CLOSE});
            } else if (now - client->close_requested_at >= 2000) {
                client->confirm_force_close = 1;
                strcpy(d->message, "应用未响应。再点关闭将强制退出，未保存内容将丢失");
            }
        } else
            close_window(d, id);
        return 1;
    }
    if (id==FILE_WIN && y>=8 && y<36 && x>=40 && x<r.w-177) {
        int end=file_crumbs(d,r,x-40);
        if (end) { char path[513]; ui_crumb_target(d->path,end,path); navigate(d,path,-1); return 1; }
    }
    if (id==FILE_WIN && !sidebar(r) && y>=8 && y<36 && x>=8 && x<38) {
        d->places_open=!d->places_open; return 1;
    }
    if (id == FILE_WIN && y >= 8 && y < 36 && x >= r.w - 172 && x < r.w - 108) {
        char parent[513];
        strcpy(parent, d->path);
        char *slash = strrchr(parent, '/');
        slash[slash == parent] = 0;
        navigate(d, parent,-1);
        return 1;
    } else if (!win->maximized && (y < TITLE || (x >= r.w - 18 && y >= r.h - 18))) {
        d->captured = id;
        d->dragging = y < TITLE ? 1 : 3;
        d->grab_x = y < TITLE ? x : r.w - x;
        d->grab_y = y < TITLE ? y : r.h - y;
        return 1;
    } else if (y < TITLE)
        return 1;
    else if (id == CALC_WIN) {
        for (int i = 0; i < COUNT; i++)
            if (contains(button_rect(r, i), ax, ay)) {
                if (keys[i] == 'q')
                    close_window(d, id);
                else
                    press(keys[i]);
                break;
            }
        return 1;
    } else if (id == WEB_WIN) {
#ifndef __TINYC__
        desk_web_pointer(ax, ay, 1, 0);
#endif
        return 1;
    } else if (id >= FIRST_CLIENT) {
        d->client_grab = id;
        return 1;
    }
    if (d->action_menu) {
        for (int a = 0; a < F_COUNT; ++a)
            if (contains(action_rect(r,a),ax,ay)) { file_action(d,a); return 1; }
        d->action_menu = 0;
    } else if ((x<sidebar(r) || (!sidebar(r) && d->places_open)) && y<r.h-FILE_FOOTER) {
        if (y<80) { if (x>=50) file_action(d,F_PIN); }
        else if (r.h>=300 && y>=r.h-FILE_FOOTER-36) file_action(d,x<56?F_BACK:F_FORWARD);
        else if ((y-80)/ROW<place_rows(r)) {
            int i=d->places->first+(y-80)/ROW;
            if (i==3) file_action(d,F_SHOW_TRASH);
            else if (i<4) navigate(d,places[i],-1);
            else if (i>=5 && i-5<d->places->count) {
                const char *path=d->places->path[i-5];
                int width=sidebar(r)?sidebar(r):r.w;
                if (x>=width-24) update_place(d,path,1);
                else navigate(d,path,-1);
            }
        }
    } else if (y >= r.h - FILE_FOOTER) {
        if (!sidebar(r) && d->places_open) d->places_open=0;
        else if (x < 76) d->action_menu = 1;
        else if (x < 138) file_action(d,F_FIND);
    } else if (y >= TITLE && y < 78 && x >= sidebar(r)) {
        int columns[FILE_COLUMNS+1];
        file_columns(r.w-sidebar(r),columns);
        int local = x-sidebar(r);
        for (int c = 0; c < FILE_COLUMNS; ++c) {
            if (local < columns[c] || local >= columns[c+1]) continue;
            d->descending = c == d->sort_column && !d->descending;
            d->sort_column = c;
            sort_files(d);
            d->first = 0;
            if (d->marked >= 0) reveal_selection(d);
            d->clicked_at = -1;
            break;
        }
    } else if (y >= 80 && x >= r.w-12 && d->count > file_rows(d)) {
        Bounds thumb = file_thumb(d,r);
        d->dragging = 4;
        d->grab_y = ay >= thumb.y && ay < thumb.y+thumb.h ? ay-thumb.y : thumb.h/2;
        int travel = r.h-80-FILE_FOOTER-thumb.h;
        scroll_files(d,ui_list_drag(y-80-d->grab_y,travel,d->count,file_rows(d)));
    } else if (y >= 80 && y < 80 + ROW*min(file_rows(d),d->count-d->first)) {
        int row = d->first + (y - 80) / ROW;
        long now = console_call(38,0);
        int twice = !d->modifiers && d->chosen==1 && d->marked == row && d->clicked_at >= 0 && now - d->clicked_at <= 550;
        select_row(d,row,d->modifiers);
        d->clicked_at = d->modifiers ? -1 : now;
        if (twice) file_action(d,F_OPEN);
    } else if (y>=80) select_row(d,-1,0);
    return 1;
}
#ifdef __TINYC__
int main(int argc, char **argv) {
    (void)argv;
    if (load_font() < 0) {
        lx_error("desktop: could not load font\n");
        return 1;
    }
    unsigned len = argc - 1;
#elif defined(LX_NETSURF)
int main(int argc, char **argv) {
    const char *name = strrchr(argv[0], '/');
    int browser = strcmp(name ? name+1 : argv[0], "desktop") != 0;
    if (browser && argc == 2 && !strcmp(argv[1], "--version")) {
        puts("NetSurf 3.11 (LixinOS framebuffer)");
        return 0;
    }
    if (argc > (browser ? 2 : 1)) { lx_error("usage: desktop | netsurf [URL]\n"); return 2; }
    char current[513], url[1040];
    const char *cwd = getcwd(current,sizeof(current)) ? current : "/";
    if (browser && argc == 2) {
        if (argv[1][0]=='/') {
            if (snprintf(url,sizeof(url),"file://%s",argv[1]) >= (int)sizeof(url)) return 2;
            lixin_initial_url(url);
        } else lixin_initial_url(argv[1]);
    }
    unsigned len = 0;
#else
extern char __data_image[], __data_begin[], __data_end[];
__asm__(".section .text.start,\"ax\"\n.global _start\n_start:\n"
        "ldr x9, =__user_heap_limit\nmov sp, x9\nb desktop_start\n.previous");
__attribute__((noreturn)) void desktop_start(const char *cwd, const char *args, unsigned long len,
                                             unsigned long flags) {
    if (flags == 65536)
        longjmp(WAITING, 1);
    memcpy(__data_begin, __data_image, __data_end - __data_begin);
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
    console_call(57, 0);
    static WindowClient clients[CLIENT_LIMIT];
    static UiPlaces favorites;
    Desktop d = {0};
    d.places=&favorites;
    memset(clients, 0, sizeof clients);
    d.clients = clients;
    d.client_grab = -1;
    d.w = size & 65535;
    d.h = size >> 16;
    restore_wallpaper(&d);
    d.pixels = (uint32_t *)0x68000000;
    int work = d.w - DOCK;
    int widths[] = {600, 850, 376}, heights[] = {391, 540, 378};
    for (int i = 0; i < APPS; i++) {
        d.order[i] = i;
        if (i >= FIRST_CLIENT) continue;
        int w = min(widths[i], work - 12), h = min(heights[i], d.h - 12);
        d.windows[i].rect = (Bounds){DOCK + (work - w) / 2, (d.h - h) / 2, w, h};
    }
    d.dialog_x = DOCK + (work - 440) / 2;
    if (d.dialog_x + 440 > d.w)
        d.dialog_x = d.w - 440;
    d.dialog_y = (d.h - 120) / 2;
#ifndef __TINYC__
    if (strlen(cwd) < sizeof(d.cwd))
        strcpy(d.cwd, cwd);
#endif
    if (!d.cwd[0])
        strcpy(d.cwd, "/");
    d.mx = d.w / 2;
    d.my = d.h / 2;
    d.inside = 1;
#ifdef LX_NETSURF
    if (browser) click(&d, DOCK/2, 8 + step(&d) + 20);
#endif
    render(&d);
    int clipboard_client = -1;
    for (;;) {
#ifdef LX_NETSURF
        if (browser && !d.windows[WEB_WIN].open) {
            for (int i = 0; i < CLIENT_LIMIT; ++i) client_shutdown(&clients[i]);
            return 0;
        }
#endif
        int transferring = 0;
        for (int i = 0; i < CLIENT_LIMIT; ++i)
            transferring |= clients[i].busy;
#ifdef LX_NETSURF
        int key = desk_web_read_key(transferring ? 1 : 8);
#else
        int key = console_call(14, transferring ? 1 : 8);
#endif
        if (d.wallpaper_menu && key>=0 && key<256) {
            d.wallpaper_menu=0; render(&d);
        } else if (d.naming && key >= 0 && key < 256) {
            int length = strlen(d.filename);
            if (key==3 || key==24) {
                int lo=min(d.name.anchor,d.name.caret), n=d.name.anchor+d.name.caret-2*lo;
                if (n) {
                    lx_copy_text(d.filename+lo,n);
                    if (key==24) ui_text_key(&d.name,d.filename,sizeof d.filename,8);
                }
            } else if (key == 27) {
                int prefix=lx_timed_byte(20);
                if (prefix=='[' || prefix=='O') {
                    int arrow=lx_timed_byte(20);
                    int move=arrow=='C'?UI_RIGHT:arrow=='D'?UI_LEFT:arrow=='H'?UI_HOME:arrow=='F'?UI_END:0;
                    if (move) ui_text_key(&d.name,d.filename,sizeof d.filename,move);
                } else { cursor(&d,0); d.naming=0; }
            } else if (key == 13 || key == 10) {
                if (d.naming == EDITOR_NAME && length) { d.naming = 0; launch(&d, -1); }
                else if (d.naming >= FILE_PROMPT) submit_file_prompt(&d);
            } else ui_text_key(&d.name,d.filename,sizeof d.filename,key);
            render(&d);
        } else if (key == 7 && !d.naming && !d.down &&
                   (d.menu || focus(&d) < FIRST_CLIENT || !clients[focus(&d) - FIRST_CLIENT].extended)) {
            show_menu(&d, d.menu ? MENU_CLOSED : MENU_APPS);
            render(&d);
        } else if (!d.menu && !d.naming && !d.down && key == 29) {
            for (int i = 0; i < APPS; ++i) {
                int id = d.order[i];
                if (d.windows[id].shown) {
                    raise_window(&d, id);
                    render(&d);
                    break;
                }
            }
        } else if (d.menu) {
            if (key == 27) { d.menu = MENU_CLOSED; render(&d); }
        } else if (focus(&d) >= FIRST_CLIENT && key >= 0 && key < 256) {
            int id = focus(&d);
            if (key == 27 && !clients[id - FIRST_CLIENT].extended) {
                close_window(&d, id);
                render(&d);
            } else
                client_event(&clients[id - FIRST_CLIENT], (WindowEvent){WINDOW_CLIENT_KEY, 0, 0, key});
        } else if (focus(&d) == CALC_WIN && key >= 0 && key < 256) {
            if (key == 'q' || key == 27)
                close_window(&d, CALC_WIN);
            else
                press(key == '\r' || key == '\n' ? '=' : key);
            render(&d);
#ifndef __TINYC__
        } else if (focus(&d) == WEB_WIN && key >= 0 && key < 256) {
            if (!desk_web_key(key))
                close_window(&d, WEB_WIN);
            render(&d);
#endif
        } else if (focus(&d) == FILE_WIN && file_key(&d,key)) {
            render(&d);
        } else if (key == 27) {
            if (!d.terminals || d.message[0] == '!') {
                for (int i = 0; i < CLIENT_LIMIT; ++i) client_shutdown(&clients[i]);
                finish(0);
            }
            strcpy(d.message, "! Open shells will close. Esc again to quit.");
            render(&d);
        }
#ifndef __TINYC__
        if (d.windows[WEB_WIN].shown && desk_web_tick())
            render(&d);
#endif
        int repaint = 0;
        for (int i = 0; i < CLIENT_LIMIT; ++i) {
            int update = client_tick(&clients[i], d.windows[FIRST_CLIENT + i].shown);
            if (update < 0) {
                if (clients[i].overflow) strcpy(d.message, "Application closed: input queue full");
                else if (clients[i].error) strcpy(d.message, clients[i].error);
                else if (!clients[i].ready) strcpy(d.message, "Application exited before its first frame");
                close_window(&d, FIRST_CLIENT + i);
                repaint = 1;
            } else if (update && d.windows[FIRST_CLIENT + i].shown && !repaint)
                repaint = !present_patch(&d, FIRST_CLIENT + i);
        }
        if (repaint && d.menu != MENU_APPS) render(&d);
        long e;
        int active = focus(&d);
        long pointer_options = d.naming && d.name.caret != d.name.anchor ? LX_COPY_ENABLED : 0;
        int target = !d.naming && !d.menu && active >= FIRST_CLIENT && clients[active - FIRST_CLIENT].extended ?
            active - FIRST_CLIENT : d.naming ? -2 : -1;
        if (target != clipboard_client) {
            console_call(35, 1);
            clipboard_client = target;
        }
        if (target >= 0) {
            pointer_options |= LX_PASTE_BLOCK | LX_COPY_ENABLED;
            client_collect_paste(&clients[target]);
        } else if (d.naming) {
            pointer_options |= LX_PASTE_BLOCK;
            long n=console_call(35,0);
            if (n>0 && n<=4096 && ui_text_paste(&d.name,d.filename,sizeof d.filename,
                    (const unsigned char *)0x443d8000UL,n)) render(&d);
        }
        while ((e = console_call(21, pointer_options | LX_CONTEXT_CUSTOM)) >= 0) {
            if (e & LX_POINTER_COPY) {
                int lo = min(d.name.anchor, d.name.caret);
                if (d.naming) lx_copy_text(d.filename + lo, d.name.anchor + d.name.caret - 2 * lo);
                else if (active>=FIRST_CLIENT) client_event(&clients[active-FIRST_CLIENT],
                    (WindowEvent){WINDOW_CLIENT_KEY,0,0,3});
            }
            if (e & (1 << 28)) {
#ifndef __TINYC__
                desk_web_cancel_pointer();
#endif
                if (d.client_grab >= FIRST_CLIENT) {
                    WindowClient *client = &clients[d.client_grab - FIRST_CLIENT];
                    client_event(client, (WindowEvent){WINDOW_CLIENT_POINTER, client->extended ? -1 : client->pointer_x, client->extended ? -1 : client->pointer_y, 0});
                    d.client_grab = -1;
                }
                d.dragging = d.down = d.name_drag = 0;
                continue;
            }
            cursor(&d, 0);
            d.mx = e & 4095;
            d.my = (e >> 12) & 4095;
            d.inside = (e >> 25) & 1;
            int down = (e >> 24) & 1, changed = 0;
            d.modifiers=(e>>32)&3;
            if (d.menu == MENU_APPS) {
                int wheel = ((e >> 26) & 1) - ((e >> 27) & 1);
                if (wheel && d.inside) {
                    changed = grid_scroll(&d,d.app_scroll-wheel*48);
                    if (down) d.app_dragged = 1;
                } else if (down && !d.down) {
                    d.app_press_x = d.mx;
                    d.app_press_y = d.my;
                    d.app_press_scroll = d.app_scroll;
                    d.app_dragged = !d.inside;
                } else if (down && d.down) {
                    if (abs(d.my-d.app_press_y) > 6 || abs(d.mx-d.app_press_x) > 6)
                        d.app_dragged = 1;
                    if (d.app_dragged && d.app_press_y >= GRID_TOP)
                        changed = grid_scroll(&d,d.app_press_scroll+d.app_press_y-d.my);
                } else if (!down && d.down && d.inside && !d.app_dragged) {
                    d.message[0] = 0;
                    menu_click(&d,d.mx,d.my);
                    changed = 1;
                }
                d.down = down;
                if (changed) render(&d);
                else cursor(&d,1);
                continue;
            }
            if(e & LX_RIGHT_PRESS) {
                d.wallpaper_menu=0;
                int id = d.inside && !d.naming && !d.menu ? window_at(&d,d.mx,d.my) : -1;
                if (id >= FIRST_CLIENT && clients[id-FIRST_CLIENT].right_click) {
                    Bounds r = d.windows[id].rect;
                    if (d.my >= r.y + TITLE) {
                        raise_window(&d, id);
                        client_event(&clients[id-FIRST_CLIENT], (WindowEvent){
                            WINDOW_CLIENT_RIGHT_CLICK, d.mx-r.x, d.my-r.y-TITLE, 0});
                        render(&d);
                        continue;
                    }
                }
                if(d.inside && !d.naming && d.mx>=DOCK && window_at(&d,d.mx,d.my)<0) {
                    d.menu=MENU_CLOSED;
                    d.wallpaper_x=clamp(d.mx,d.w-WALL_WIDTH);
                    d.wallpaper_y=clamp(d.my,d.h-WALL_HEIGHT);
                    d.wallpaper_menu=1;
                    render(&d);
                } else if (d.inside && !d.naming && !d.menu && window_at(&d,d.mx,d.my)==FILE_WIN) {
                    Bounds r=d.windows[FILE_WIN].rect;
                    int y=d.my-r.y;
                    if (y>=80 && y<r.h-FILE_FOOTER && d.mx>=r.x+sidebar(r)) {
                        int row=d.first+(y-80)/ROW;
                        if (row>=d.count) select_row(&d,-1,0);
                        else if (!d.rows[row].chosen) select_row(&d,row,0);
                    }
                    raise_window(&d,FILE_WIN); d.action_menu=1; d.clicked_at=-1; render(&d);
                } else {
                    render(&d);
                    if(d.inside) console_call(21, ((e & 0xffffffUL)<<32) | LX_CONTEXT_EDIT | pointer_options);
                }
                continue;
            }
            int was_modal = d.menu || d.naming || d.wallpaper_menu;
            if(d.wallpaper_menu && down && !d.down) {
                int x=d.mx-d.wallpaper_x,y=d.my-d.wallpaper_y;
                if(d.inside && x>=0 && x<WALL_WIDTH && y>=WALL_ROW && y<WALL_HEIGHT) {
                    int choice = y/WALL_ROW-1;
                    d.message[0] = 0;
                    if (choice == WALL_COUNT) {
                        d.naming = WALL_PROMPT;
                        strcpy(d.filename,d.wall_file);
                        name_begin(&d,1);
                    } else {
                        char value = '0'+choice;
                        if (lx_save_file(wall_path,&value,1) < 0)
                            strcpy(d.message,"无法保存壁纸设置");
                        else {
                            free(d.wall_image);
                            d.wall_image = NULL;
                            d.wall_file[0] = 0;
                            d.wallpaper = choice;
                        }
                    }
                }
                d.wallpaper_menu=0; d.down=1; render(&d); continue;
            }
            if (d.naming && d.name_drag) {
                if (down || d.inside) d.name.caret = name_position(&d, d.mx);
                d.name_drag = down;
                changed = 1;
            } else if (d.dragging) {
                if (d.dragging == 4) {
                    Bounds r = d.windows[FILE_WIN].rect;
                    if (d.count > file_rows(&d)) {
                        int travel = r.h-80-FILE_FOOTER-file_thumb(&d,r).h;
                        int offset = clamp(d.my-r.y-80-d.grab_y,travel);
                        scroll_files(&d,ui_list_drag(offset,travel,d.count,file_rows(&d)));
                    }
                } else if (d.dragging == 2) {
                    d.dialog_x = clamp(d.mx - d.grab_x, d.w - 440);
                    d.dialog_y = clamp(d.my - d.grab_y, d.h - 120);
                } else {
                    Bounds *r = &d.windows[d.captured].rect;
                    if (d.dragging == 1) {
                        r->x = DOCK + clamp(d.mx - d.grab_x - DOCK, d.w - r->w - DOCK);
                        r->y = clamp(d.my - d.grab_y, d.h - r->h);
                    } else {
                        int old_w = r->w, old_h = r->h;
                        int minw = min(320, d.w - r->x), minh = min(250, d.h - r->y);
                        r->w = minw + clamp(d.mx - r->x + d.grab_x - minw, d.w - r->x - minw);
                        r->h = minh + clamp(d.my - r->y + d.grab_y - minh, d.h - r->y - minh);
                        if (r->w != old_w || r->h != old_h) configure_window(&d, d.captured);
                        if (d.captured == FILE_WIN) scroll_files(&d,d.first);
                    }
                }
                changed = 1;
                if (!down)
                    d.dragging = 0;
            } else if (d.inside && down && !d.down)
                changed = click(&d, d.mx, d.my);
            else if (!was_modal && d.client_grab < 0 && d.inside) {
                int id = window_at(&d, d.mx, d.my);
                int wheel = ((e >> 26) & 1) - ((e >> 27) & 1);
                if (id == FILE_WIN && wheel && !d.action_menu) {
                    Bounds r=d.windows[id].rect;
                    if (d.mx-r.x<sidebar(r) || (!sidebar(r) && d.places_open)) {
                        d.places->first=ui_list_top(d.places->first-wheel,place_count(&d),place_rows(r));
                        changed=1;
                    } else changed=scroll_files(&d,d.first-wheel);
                }
#ifndef __TINYC__
                if (id == WEB_WIN && (wheel || (!down && d.down)) && focus(&d) == WEB_WIN &&
                    d.my >= d.windows[WEB_WIN].rect.y + TITLE)
                    changed = desk_web_pointer(d.mx, d.my, down, wheel);
#endif
            }
#ifndef __TINYC__
            if (!down && d.down)
                desk_web_cancel_pointer();
#endif
            int target = d.client_grab;
            if (target < 0 && !down && !d.down && !d.dragging && !was_modal && d.inside)
                target = window_at(&d, d.mx, d.my);
            if (target >= FIRST_CLIENT) {
                Bounds r = d.windows[target].rect;
                if (d.client_grab == target || d.my >= r.y + TITLE) {
                    int adaptive = clients[target - FIRST_CLIENT].adaptive;
                    int w = adaptive ? r.w : WINDOW_CLIENT_WIDTH;
                    int h = adaptive ? r.h - TITLE : WINDOW_CLIENT_HEIGHT;
                    int x = clamp((d.mx - r.x) * w / r.w, w - 1);
                    int y = clamp((d.my - r.y - TITLE) * h / (r.h - TITLE), h - 1);
                    if (clients[target-FIRST_CLIENT].extended && (!d.inside || d.mx<r.x || d.mx>=r.x+r.w ||
                        d.my<r.y+TITLE || d.my>=r.y+r.h)) x=y=-1;
                    client_event(&clients[target - FIRST_CLIENT], (WindowEvent){WINDOW_CLIENT_POINTER, x, y, down});
                    int wheel = ((e >> 26) & 1) - ((e >> 27) & 1);
                    if (wheel && clients[target - FIRST_CLIENT].extended)
                        client_event(&clients[target - FIRST_CLIENT], (WindowEvent){WINDOW_CLIENT_WHEEL, x, y, wheel});
                }
            }
            if (!down) d.client_grab = -1;
            d.down = down;
            if (changed)
                render(&d);
            else
                cursor(&d, 1);
        }
    }
}
