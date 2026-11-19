#include "native.h"

/* QEMU ramfb XRGB, 2 physical pixels per logical canvas pixel. */
enum {
    MAX_WIDTH = 4096,
    MAX_HEIGHT = 2304,
    BAR = 44,
    CW = 22,
    CH = 44,
    MAX_COLS = MAX_WIDTH / CW,
    MAX_ROWS = MAX_HEIGHT / CH - 1,
    HISTORY = 256
};
enum { FG = 0xdadbd5, BG = 0x171b20, BAR_BG = 0x273038, BAR_CLOCK = 0xd4e3ff };
static u32 fb[MAX_HEIGHT * MAX_WIDTH] __attribute__((aligned(4096)));
static u32 *frame[MAX_HEIGHT];
static unsigned width = 2354, height = 1408, pane_width = 2354, columns = 107, rows = 31, origin,
                selector;
static int split, online, ssh;
static int clock_offset;
static void paint_status(void);
int ssh_mode(void) { return ssh; }
static unsigned focus;
int split_mode(void) { return split; }
unsigned focused_pane(void) { return focus; }
void cycle_focus(void) {
    if (split)
        focus ^= 1;
}
static u8 font[95 * CW * CH];
struct cell {
    u8 byte, reverse;
    u32 color, paper;
};
struct terminal {
    struct cell cells[MAX_ROWS][MAX_COLS], history[HISTORY][MAX_COLS];
    unsigned history_head, history_count, scroll, reverse, cursor;
    u8 wrapped[MAX_ROWS];
    unsigned row, col, escape, nparam, params[16], prefix, saved_x, saved_y;
    unsigned margin_start, margin_end, app_cursor, nowrap, charsets[2], active_set;
    unsigned charset_bank, osc, string_esc, scalar;
    unsigned anchor, extent, selecting, selected;
    u32 color, paper, utf8;
    unsigned agent_bar;
    char agent_root[513];
    struct terminal *previous;
};
static struct terminal panes[2], *term = panes;
/* The desktop draws its localized title in EL0. Keep one terminal grid per
 * suspended shell and a clean desktop backing while its window is visible. */
struct popup {
    struct terminal *grid;
    u32 *backdrop, *title;
    unsigned x, y, w, h, cols, rows, screen_w, screen_h;
    int editor, drag, dx, dy, down, closing;
};
static struct popup windows[NATIVE_TASKS], *shown[2], *win;
static unsigned top = BAR, pane_origin;
static void choose_view(unsigned id) {
    pane_origin = id * (pane_width + CW);
    win = shown[id];
    origin = pane_origin + (win ? 2 * (win->x + 6) : 0);
    top = BAR + (win ? 2 * (win->y + 44) : 0);
    columns = win ? win->cols : pane_width / CW;
    rows = win ? win->rows : height / CH - 1;
    term = win ? win->grid : &panes[id];
    if (term->agent_bar && rows >= 4) {rows -= 2; top += 2*CH;}
}
static void pixel(unsigned x, unsigned y, u32 color) {
    u32 *d = &frame[BAR + 2 * y][pane_origin + 2 * x];
    d[0] = d[1] = d[width] = d[width + 1] = color;
}
static void paint_window(void) {
    for (unsigned j = 0; j < win->h; j++)
        for (unsigned i = 0; i < win->w; i++) {
            u32 color = j < 44                                        ? win->title[j * win->w + i]
                        : j >= win->h - 6 || i < 6 || i >= win->w - 6 ? 0x233c3d
                                                                      : BG;
            pixel(win->x + i, win->y + j, color);
        }
    display_restore();
}
static int show_window(struct popup *w) {
    w->screen_w = pane_width / 2;
    w->screen_h = (height - BAR) / 2;
    w->backdrop = (u32 *)file_buffer(16 * w->screen_w * w->screen_h);
    if (!w->backdrop)
        return 0;
    for (unsigned j = 0; j < 2 * w->screen_h; j++)
        memcpy(w->backdrop + j * 2 * w->screen_w, frame[BAR + j] + pane_origin, 8 * w->screen_w);
    shown[process_pane()] = w;
    choose_view(process_pane());
    paint_window();
    return 1;
}
int popup_open(int slot, unsigned title_w, const u32 *title, int editor) {
    struct popup *w = &windows[slot];
    unsigned cols = columns > 8 ? columns - 8 : 1;
    if (cols > 76)
        cols = 76;
    unsigned h = (height - BAR) / 2;
    unsigned nrows = h > 50 ? (h - 50) / 22 : 0;
    if (nrows > 21)
        nrows = 21;
    if (!nrows || title_w != cols * 11 + 12 || shown[process_pane()])
        return 0;
    *w = (struct popup){
        .w = title_w, .h = nrows * 22 + 50, .cols = cols, .rows = nrows, .editor = editor};
    w->x = (pane_width / 2 - w->w) / 2;
    w->y = (h - w->h) / 2;
    w->grid = (struct terminal *)file_buffer(sizeof(*w->grid));
    w->title = (u32 *)file_buffer(4 * w->w * 44);
    if (w->grid && w->title) {
        memset(w->grid, 0, sizeof(*w->grid));
        w->grid->color = FG;
        w->grid->cursor = 1;
        for (unsigned i = 0; i < 44; i++)
            memcpy(w->title + w->w * i, title + pane_width / 2 * i, 4 * w->w);
        if (show_window(w))
            return 1;
    }
    release_buffer((const u8 *)w->grid, sizeof(*w->grid));
    release_buffer((const u8 *)w->title, 4 * w->w * 44);
    w->grid = 0;
    return 0;
}
int popup_show(int slot) { return show_window(&windows[slot]); }
void popup_hide(int slot, int keep) {
    struct popup *w = &windows[slot];
    if (!w->grid)
        return;
    if (w->backdrop) {
        for (unsigned j = 2*w->y; j < 2*(w->y + w->h); j++)
            memcpy(frame[BAR+j] + pane_origin + 2*w->x,
                   w->backdrop + j*2*w->screen_w + 2*w->x, 8*w->w);
        release_buffer((const u8 *)w->backdrop, 16 * w->screen_w * w->screen_h);
        w->backdrop = 0;
    }
    shown[process_pane()] = 0;
    choose_view(process_pane());
    if (!keep) {
        if (w->grid->previous)
            release_buffer((const u8 *)w->grid->previous, sizeof(*w->grid));
        release_buffer((const u8 *)w->grid, sizeof(*w->grid));
        release_buffer((const u8 *)w->title, 4 * w->w * 44);
        w->grid = 0;
    }
}
static int clamp(int x, int max) { return x < 0 ? 0 : x > max ? max : x; }
static void move_window(unsigned x, unsigned y) {
    unsigned ox = win->x, oy = win->y, w = win->w, h = win->h;
    if (x == ox && y == oy)
        return;
    for (unsigned k = 0; k < 2 * h; k++) {
        unsigned j = y > oy ? 2 * h - 1 - k : k;
        memmove(frame[BAR + 2 * y + j] + pane_origin + 2 * x,
                frame[BAR + 2 * oy + j] + pane_origin + 2 * ox, w * 8);
    }
    for (unsigned j = 2*oy; j < 2*(oy+h); j++) {
        unsigned left = 2*ox, right = 2*(ox+w);
        if (j >= 2*y && j < 2*(y+h)) {
            if (x > ox && x < ox+w) right = 2*x;
            else if (x < ox && x+w > ox) left = 2*(x+w);
            else if (x == ox) continue;
        }
        memcpy(frame[BAR+j] + pane_origin + left, win->backdrop + j*2*win->screen_w + left,
               4*(right-left));
    }
    win->x = x;
    win->y = y;
    choose_view(process_pane());
}
int popup_closed(void) { return win && win->closing && !win->editor; }
int popup_key(int key) {
    if (!win)
        return key;
    if (key == 29 && !win->editor && win == &windows[task_active()]) {
        task_pause();
        return -1;
    }
    if (win->closing) {
        if (win->editor) {
            win->closing = 0;
            return 17;
        }
        return win == &windows[task_active()] ? 256 : 4;
    }
    return key;
}
static long popup_pointer(long e) {
    if (!win || e < 0)
        return e;
    int x = e & 4095, y = (e >> 12) & 4095, down = (e >> 24) & 1, inside = (e >> 25) & 1;
    int dx = x - (int)win->x, dy = y - (int)win->y;
    if (win->drag) {
        move_window(clamp(x - win->dx, win->screen_w - win->w),
                    clamp(y - win->dy, win->screen_h - win->h));
        win->drag = down;
        win->down = down;
        return -1;
    }
    int press = down && !win->down;
    win->down = down;
    if (press && inside && dx >= 0 && dx < (int)win->w && dy >= 0 && dy < 44) {
        if (dy >= 8 && dy < 36 && dx >= (int)win->w - 35 && dx < (int)win->w - 7)
            win->closing = 1;
        else if (!win->editor && dy >= 8 && dy < 36 && dx >= (int)win->w - 68 &&
                 dx < (int)win->w - 40 && win == &windows[task_active()]) {
            task_pause();
        } else {
            win->drag = 1;
            win->dx = dx;
            win->dy = dy;
        }
        return -1;
    }
    inside = inside && dx >= 6 && dx < (int)win->w - 6 && dy >= 44 && dy < (int)win->h - 6;
    return (e & ~(0x3ffffffUL)) | ((long)inside << 25) | ((long)down << 24) |
           (clamp(dy - 44, (int)win->h - 51) << 12) | clamp(dx - 6, (int)win->w - 13);
}

int display_application_cursor(void) {
    struct terminal *t=shown[focus]?shown[focus]->grid:&panes[focus];
    return t->app_cursor;
}
void display_select(unsigned id) { choose_view(id); }
static u32 be32(u32 x) {
    return (x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24);
}
static u64 be64(u64 x) { return ((u64)be32(x) << 32) | be32(x >> 32); }

static void glyph(unsigned x, unsigned y, u8 c, u32 ink, u32 paper) {
    if (c < 32 || c >= 127)
        c = '?';
    const u8 *src = font + (c - 32) * CW * CH;
    for (unsigned row = 0; row < CH; row++) {
        u32 *dst = frame[y + row] + x;
        for (unsigned col = 0; col < CW; col++) {
            unsigned a = *src++;
            if (!a || a == 255) {
                dst[col] = a ? ink : paper;
                continue;
            }
            unsigned rgb = 0;
            for (unsigned shift = 0; shift <= 16; shift += 8) {
                unsigned fg = (ink >> shift) & 255, bg = (paper >> shift) & 255;
                rgb |= ((fg * a + bg * (255 - a)) / 255) << shift;
            }
            dst[col] = rgb;
        }
    }
}
static void draw_cell(unsigned r, unsigned c) {
    const struct cell *cell =
        r < term->scroll
            ? &term->history[(term->history_head + HISTORY - term->scroll + r) % HISTORY][c]
            : &term->cells[r - term->scroll][c];
    unsigned ink = cell->color ? cell->color : FG, paper = cell->paper ? cell->paper : BG;
    unsigned pos = r * columns + c;
    unsigned lo = term->anchor < term->extent ? term->anchor : term->extent;
    unsigned hi = term->anchor + term->extent - lo;
    int inverse = cell->reverse ^ (term->selected && pos >= lo && pos <= hi);
    glyph(origin + c * CW, top + r * CH, cell->byte ? cell->byte : ' ', inverse ? paper : ink,
          inverse ? ink : paper);
}
static void agent_header(void) {
    if (!term->agent_bar || (win ? win->rows : height / CH - 1) < 4) return;
    unsigned y = top - 2*CH;
    const char *title = columns >= 27 ? " LixinOS Agent " : " Agent ";
    const char *badge = term->agent_bar == 1 ? " Bypass on " : " Bypass off ";
    unsigned badge_col = columns > strlen(badge) ? columns-strlen(badge) : 0;
    const char *root = term->agent_root;
    unsigned len = strlen(root);
    if (len > columns-2) root += len - (columns-2);
    for (unsigned c=0; c<columns; c++) {
        int right = c>=badge_col;
        unsigned bg = !right ? BAR_BG : term->agent_bar == 1 ? 0xffdf9e : 0xbde5ba;
        char ch = right ? badge[c-badge_col] : c<strlen(title) ? title[c] : ' ';
        glyph(origin+c*CW,y,ch,right ? BG : 0xa5edeb,bg);
        glyph(origin+c*CW,y+CH,c && c-1<strlen(root) ? root[c-1] : ' ',FG,0x20272e);
    }
}
void display_restore(void) {
    if (!online)
        return;
    agent_header();
    for (unsigned r = 0; r < rows; r++)
        for (unsigned c = 0; c < columns; c++)
            draw_cell(r, c);
}
static int configure(void) {
    volatile u8 *fw = (volatile u8 *)0x09020000UL;
    struct {
        u64 address;
        u32 fourcc, flags, width, height, stride;
    } info;
    info.address = be64((u64)fb);
    info.fourcc = be32(0x34325258);
    info.flags = 0;
    info.width = be32(width);
    info.height = be32(height);
    info.stride = be32(width * 4);
    struct {
        volatile u32 control;
        u32 length;
        u64 addr;
    } dma;
    dma.control = be32((selector << 16) | 24);
    dma.length = be32(28);
    dma.addr = be64((u64)&info);
    asm volatile("dsb sy" ::: "memory");
    *(volatile u64 *)(fw + 16) = be64((u64)&dma);
    while (be32(dma.control) & ~1u) {
    }
    return !dma.control;
}
void display_init(void) {
    volatile u8 *fw = (volatile u8 *)0x09020000UL;
    *(volatile u16 *)(fw + 8) = 0x1900;
    u32 count = 0;
    for (unsigned i = 0; i < 4; i++)
        count = count << 8 | *fw;
    unsigned mode = 0, clock = 0;
    for (unsigned i = 0; i < count && i < 65536; i++) {
        u8 buf[64];
        for (unsigned j = 0; j < 64; j++)
            buf[j] = *fw;
        unsigned j = 0;
        const char *name = "etc/ramfb";
        while (name[j] && buf[8 + j] == name[j])
            j++;
        if (!name[j] && !buf[8 + j])
            selector = buf[4] * 256u + buf[5];
        if (!memcmp(buf + 8, "opt/lixinos/mode", 17))
            mode = buf[4] * 256u + buf[5];
        if (!memcmp(buf + 8, "opt/lixinos/clock-offset", sizeof("opt/lixinos/clock-offset")) &&
            (buf[0] || buf[1] || buf[2] || buf[3] >= 5))
            clock = buf[4] * 256u + buf[5];
    }
    if (mode) {
        *(volatile u16 *)(fw + 8) = (u16)(mode >> 8 | mode << 8);
        char value[5];
        for (unsigned i = 0; i < 5; i++)
            value[i] = *fw;
        split = !memcmp(value, "split", 5);
        ssh = !memcmp(value, "ssh", 3);
    }
    if (clock) {
        *(volatile u16 *)(fw + 8) = (u16)(clock >> 8 | clock << 8);
        int sign = *fw;
        int digits[4], valid = sign == '+' || sign == '-';
        for (unsigned i = 0; i < 4; i++) {
            digits[i] = (int)*fw - '0';
            valid &= digits[i] >= 0 && digits[i] <= 9;
        }
        int hours = digits[0] * 10 + digits[1], minutes = digits[2] * 10 + digits[3];
        if (valid && hours < 24 && minutes < 60)
            clock_offset = (hours * 3600 + minutes * 60) * (sign == '-' ? -1 : 1);
    }
    if (!selector)
        return;
    if (split) {
        width = 3014;
        pane_width = (width - CW) / 2;
        columns = pane_width / CW;
    }
    for (unsigned i = 0; i < 2; i++) {
        panes[i].cursor = 1;
        panes[i].color = FG;
    }
    for (unsigned y = 0; y < height; y++)
        frame[y] = fb + y * width;
    struct file f;
    if (!lookup("/src/gui/console-menlo.gray", &f) || f.size != sizeof(font))
        return;
    memcpy(font, f.data, sizeof(font));
    if (!configure())
        return;
    online = 1;
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++)
            frame[y][x] =
                y < BAR || (split && x >= pane_width && x < pane_width + CW) ? BAR_BG : BG;
    paint_status();
}
static void reflow(struct terminal *t, unsigned oldcols, unsigned oldrows) {
    static struct cell cells[MAX_ROWS][MAX_COLS];
    u8 soft[MAX_ROWS] = {0};
    unsigned y = 0, x = 0, cy = 0, cx = 0, last = t->row;
    for (unsigned r = t->row + 1; r < oldrows; r++)
        for (unsigned c = 0; c < oldcols; c++)
            if (t->cells[r][c].byte)
                last = r;
    memset(cells, 0, sizeof(cells));
    for (unsigned r = 0; r <= last; r++) {
        unsigned end = oldcols;
        if (!t->wrapped[r]) {
            while (end && !t->cells[r][end - 1].byte)
                end--;
            if (r == t->row && end < t->col)
                end = t->col;
        }
        for (unsigned c = 0; c <= end; c++) {
            if (x == columns && c < end) {
                soft[y] = 1;
                x = 0;
                y++;
            }
            if (y == rows) {
                memmove(cells, cells[1], (rows - 1) * sizeof(cells[0]));
                memset(cells[rows - 1], 0, sizeof(cells[0]));
                memmove(soft, soft + 1, rows - 1);
                soft[rows - 1] = 0;
                y--;
                if (cy)
                    cy--;
            }
            if (r == t->row && c == t->col) {
                cy = y;
                cx = x;
            }
            if (c < end)
                cells[y][x++] = t->cells[r][c];
        }
        if (!t->wrapped[r]) {
            x = 0;
            y++;
        }
    }
    memcpy(t->cells, cells, sizeof(cells));
    memcpy(t->wrapped, soft, sizeof(soft));
    t->col = cx;
    t->row = cy;
    t->scroll = t->selected = t->selecting = 0;
}
void display_agent_bar(unsigned mode, const char *root) {
    if (!online || mode>2 || (!mode && !term->agent_bar)) return;
    unsigned old_rows=rows;
    term->agent_bar=mode;
    unsigned n=0;
    while (root[n] && n<512) {
        term->agent_root[n] = root[n]>=32 && root[n]<=126 ? root[n] : '?';
        n++;
    }
    term->agent_root[n]=0;
    choose_view(process_pane());
    if (rows!=old_rows) reflow(term,columns,old_rows);
    display_restore();
}
static unsigned requested_w, requested_h;
void resize_display(unsigned w, unsigned h) {
    requested_w = w;
    requested_h = h;
}
static void apply_resize(void) {
    if (!requested_w || !requested_h || display_busy())
        return;
    unsigned w = requested_w, h = requested_h, min = (split ? 25 : 12) * CW;
    requested_w = requested_h = 0;
    if (w < min)
        w = min;
    if (w > MAX_WIDTH)
        w = MAX_WIDTH;
    if (h < 6 * CH)
        h = 6 * CH;
    if (h > MAX_HEIGHT)
        h = MAX_HEIGHT;
    if (w == width && h == height)
        return;
    unsigned oldcols = pane_width / CW, previous_rows = height / CH - 1;
    width = w;
    height = h;
    columns = (width / CW - split) / (split + 1);
    pane_width = columns * CW;
    rows = height / CH - 1;
    for (unsigned y = 0; y < height; y++)
        frame[y] = fb + y * width;
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++)
            frame[y][x] =
                y < BAR || (split && x >= pane_width && x < pane_width + CW) ? BAR_BG : BG;
    for (unsigned i = 0; i <= (unsigned)split; i++) {
        display_select(i);
        unsigned oldrows = previous_rows - (term->agent_bar && previous_rows>=4 ? 2 : 0);
        reflow(term, oldcols, oldrows);
        display_restore();
    }
    display_select(process_pane());
    paint_status();
    configure();
}
static void paint_status(void) {
    for (unsigned y = 0; y < BAR; y++)
        for (unsigned x = 0; x < width; x++)
            frame[y][x] = BAR_BG;

    unsigned length = width >= 26 * CW ? 8 : 5;
    unsigned clock_x = width - (length + 1) * CW;
    unsigned utc = *(volatile u32 *)0x09010000UL % 86400;
    unsigned local = ((int)utc + 86400 + clock_offset) % 86400;
    unsigned h = local / 3600, m = local / 60 % 60, s = local % 60;
    char time[] = {'0' + h / 10, '0' + h % 10, ':', '0' + m / 10,
                   '0' + m % 10, ':', '0' + s / 10, '0' + s % 10};
    for (unsigned i = 0; i < length; i++)
        glyph(clock_x + i * CW, 0, time[i], BAR_CLOCK, BAR_BG);
    if (clock_x >= 9 * CW) {
        const char *title = "LixinOS";
        for (unsigned i = 0; title[i]; i++)
            glyph((i + 1) * CW, 0, title[i], FG, BAR_BG);
    }

    char buf[80];
    char *p = add_number(add_text(buf, "CPU "), cpu_percent());
    p = add_number(add_text(p, "%   Heap "), heap_bytes() >> 20);
    p = add_number(add_text(p, " MiB   Disk "), disk_usage() >> 20);
    p = add_text(add_number(add_text(p, " / "), SNAPSHOT_SLOT_BYTES >> 20), " MiB");
    unsigned metrics = p - buf;
    if ((14 + metrics + 1) * CW <= clock_x)
        for (unsigned i = 0; i < metrics; i++)
            glyph((14 + i) * CW, 0, buf[i], FG, BAR_BG);
}
void display_status(void) {
    static u64 last;
    if (!online)
        return;
    apply_resize();
    u64 now = clock_ticks();
    if (now - last < clock_hz() / 2)
        return;
    last = now;
    paint_status();
}
static void terminal_pointer(long e) {
    if (!(e & (3UL << 26))) {
        unsigned x = (e & 4095) * 2 / CW, y = ((e >> 12) & 4095) * 2 / CH;
        int down = (e >> 24) & 1;
        if ((e & (1UL << 25)) && x < columns && y < rows && (down || term->selecting)) {
            if (down && !term->selecting) {
                term->anchor = y * columns + x;
                term->selected = 1;
            }
            term->extent = y * columns + x;
            display_restore();
        }
        term->selecting = down;
        return;
    }
    unsigned old = term->scroll;
    if (e & (1UL << 26))
        term->scroll =
            term->scroll + 3 < term->history_count ? term->scroll + 3 : term->history_count;
    if (e & (1UL << 27))
        term->scroll = term->scroll < 3 ? 0 : term->scroll - 3;
    if (old != term->scroll)
        display_restore();
}
void display_idle_pointer(void) {
    if (!online || process_pane() != focus || handles_pointer())
        return;
    long e;
    while ((e = canvas_pointer()) >= 0) {
        e = popup_pointer(e);
        if (e >= 0)
            terminal_pointer(e);
    }
}
void selection_copy(void) {
    display_idle_pointer();
    if (!online || !term->selected)
        return;
    unsigned lo = term->anchor < term->extent ? term->anchor : term->extent,
             hi = term->anchor + term->extent - lo;
    u8 bytes[4096];
    unsigned n = 0;
    for (unsigned pos = lo; pos <= hi && n < sizeof(bytes); pos++) {
        unsigned r = pos / columns, c = pos % columns;
        if (c == 0 && pos != lo && n < sizeof(bytes) - 1)
            bytes[n++] = '\n';
        const struct cell *cell =
            r < term->scroll
                ? &term->history[(term->history_head + HISTORY - term->scroll + r) % HISTORY][c]
                : &term->cells[r - term->scroll][c];
        bytes[n++] = cell->byte ? cell->byte : ' ';
    }
    clipboard_copy(bytes, n);
}
int editor_input(void) {
    static int pressed;
    for (;;) {
        long e = popup_pointer(canvas_pointer());
        if (e >= 0) {
            int down = (e >> 24) & 1, inside = (e >> 25) & 1;
            if ((down && inside) || pressed) {
                int phase = down ? (pressed ? 2 : 1) : 3;
                pressed = down;
                return phase << 20 | (((e >> 12) & 4095) * 2 / CH) << 8 | (e & 4095) * 2 / CW;
            }
        }
        int key = popup_key(console_key());
        if (key >= 0)
            return key;
        net_poll();
        idle_slice();
    }
}
static void next_row(int soft) {
    term->wrapped[term->row] = soft;
    term->col = 0;
    if (term->row + 1 < rows) {
        term->row++;
        return;
    }
    memcpy(term->history[term->history_head], term->cells[0], sizeof(term->cells[0]));
    term->history_head = (term->history_head + 1) % HISTORY;
    if (term->history_count < HISTORY)
        term->history_count++;
    memmove(term->wrapped, term->wrapped + 1, rows - 1);
    term->wrapped[rows - 1] = 0;
    memmove(term->cells, term->cells[1], (rows - 1) * sizeof(term->cells[0]));
    memset(term->cells[rows - 1], 0, sizeof(term->cells[0]));
    for (unsigned y = top; y < top + (rows - 1) * CH; y++)
        memcpy(frame[y] + origin, frame[y + CH] + origin, columns * CW * 4);
    for (unsigned c = 0; c < columns; c++)
        draw_cell(rows - 1, c);
}

static void style(unsigned n) {
    static const u32 colors[] = {0x565e6b, 0xe69a96, 0x9dc69e, 0xe7cc91, 0x90b7ed, 0xcdabd9,
                                 0x89c9ca, FG,       0x88939b, 0xffb2ae, 0xbde5ba, 0xffdf9e,
                                 0xb3cfff, 0xe9cfed, 0xa5edeb, 0xffffff};
    if (!n) {
        term->color = FG;
        term->paper = BG;
        term->reverse = 0;
    }
    if (n == 39)
        term->color = FG;
    if (n == 49)
        term->paper = BG;
    if (n >= 30 && n <= 37)
        term->color = colors[n - 30];
    if (n >= 90 && n <= 97)
        term->color = colors[n - 90 + 8];
    if (n >= 40 && n <= 47)
        term->paper = colors[n - 40];
    if (n >= 100 && n <= 107)
        term->paper = colors[n - 100 + 8];
    if (n == 7)
        term->reverse = 1;
    if (n == 27)
        term->reverse = 0;
}
static void alternate(int enable) {
    if (enable && !term->previous) {
        struct terminal *saved = (struct terminal *)file_buffer(sizeof(*term));
        if (!saved)
            return;
        memcpy(saved, term, sizeof(*term));
        memset(term, 0, sizeof(*term));
        term->cursor = 1;
        term->color = FG;
        term->previous = saved;
    } else if (!enable && term->previous) {
        struct terminal *saved = term->previous;
        memcpy(term, saved, sizeof(*term));
        release_buffer((u8 *)saved, sizeof(*term));
    } else
        return;
    display_restore();
}
static void erase_cells(unsigned r, unsigned from, unsigned end) {
    for (unsigned c=from; c<end; c++) {
        term->cells[r][c]=(struct cell){0,term->reverse,term->color,term->paper};
        draw_cell(r,c);
    }
}
static void shift_rows(unsigned first, unsigned last, unsigned n, int down) {
    if (first>last || last>=rows) return;
    unsigned count=last-first+1;
    if (n>count) n=count;
    unsigned keep=count-n, src=first+(down?0:n), dst=first+(down?n:0);
    if (keep) memmove(term->cells[dst],term->cells[src],keep*sizeof(term->cells[0]));
    memmove(term->wrapped+dst,term->wrapped+src,keep);
    for (unsigned r=first; r<=last; r++) {
        if ((down && r<first+n) || (!down && r >= first+keep)) {
            term->wrapped[r]=0;
            erase_cells(r,0,columns);
        } else for (unsigned c=0;c<columns;c++) draw_cell(r,c);
    }
}
static void feed(int reverse, int soft) {
    unsigned last=term->margin_end && term->margin_end<rows ? term->margin_end : rows-1;
    unsigned first=term->margin_start<last ? term->margin_start : 0;
    if (reverse) {
        if (term->row==first) shift_rows(first,last,1,1);
        else if (term->row) term->row--;
    } else if (term->row==last && (first || last!=rows-1 || term->previous)) {
        shift_rows(first,last,1,0);
    } else if (term->row<rows-1 || term->row==last) {
        unsigned col=term->col;
        next_row(soft);
        term->col=col;
    }
}
static void run_csi(u8 c) {
    unsigned a=term->params[0], b=term->params[1], n=a?a:1;
    unsigned x=term->col<columns ? term->col : columns-1, y=term->row;
    unsigned first=term->margin_start, last=term->margin_end?term->margin_end:rows-1;
    if (last>=rows || first>=last) {first=0;last=rows-1;}
    if (term->prefix) {
        if (term->prefix!='?' || (c!='h' && c!='l')) return;
        int on=c=='h';
        for (unsigned i=0;i<=term->nparam;i++) {
            unsigned v=term->params[i];
            if (v==1) term->app_cursor=on;
            if (v==7) {term->nowrap=!on;term->col=x;}
            if (v==25) term->cursor=on;
            if (v==1048) {
                if (on) {term->saved_x=x;term->saved_y=y;}
                else {term->col=clamp(term->saved_x,columns-1);term->row=clamp(term->saved_y,rows-1);}
            }
            if (v==47 || v==1047 || v==1049) {alternate(on);return;}
        }
        return;
    }
    if (c!='m') term->col=x;
    switch (c) {
    case 'A': case 'F': term->row=y>n?y-n:0; if(c=='F')term->col=0; break;
    case 'B': case 'e': case 'E': term->row=clamp(y+n,rows-1);if(c=='E')term->col=0;break;
    case 'C': case 'a': term->col=clamp(x+n,columns-1);break;
    case 'D': term->col=x>n?x-n:0;break;
    case 'G': case '`': term->col=clamp(n-1,columns-1);break;
    case 'd': term->row=clamp(n-1,rows-1);break;
    case 'H': case 'f': term->col=clamp(b?b-1:0,columns-1);term->row=clamp(n-1,rows-1);break;
    case 's': term->saved_x=x;term->saved_y=y;break;
    case 'u': term->col=clamp(term->saved_x,columns-1);term->row=clamp(term->saved_y,rows-1);break;
    case 'J':
        if(a==3 && !term->previous){term->history_count=term->history_head=term->scroll=0;break;}
        if(a>2)break;
        for(unsigned r=0;r<rows;r++) if(a==2 || (!a && r>=y) || (a==1 && r<=y)){
            erase_cells(r,!a && r==y ? x : 0,a==1 && r==y ? x+1 : columns);
            if(a==2 || r!=y)term->wrapped[r]=0;
        }
        break;
    case 'K': if(a<3)erase_cells(y,a?0:x,a==1?x+1:columns);break;
    case 'X': erase_cells(y,x,clamp(x+n,columns));break;
    case '@': case 'P': {
        if(n>columns-x)n=columns-x;
        struct cell *line=term->cells[y];
        memmove(line+x+(c=='@'?n:0),line+x+(c=='P'?n:0),(columns-x-n)*sizeof(*line));
        unsigned blank=c=='@'?x:columns-n;
        erase_cells(y,blank,blank+n);
        for(unsigned k=x;k<columns;k++)draw_cell(y,k);
        break;
    }
    case 'L': case 'M': if(y>=first && y<=last)shift_rows(y,last,n,c=='L');break;
    case 'T': case 'S': shift_rows(first,last,n,c=='T');break;
    case 'r': if (!b)b=rows;if(n<b && b<=rows){term->margin_start=n-1;term->margin_end=b-1;term->col=term->row=0;}break;
    case 'm': for(unsigned i=0;i<=term->nparam;i++)style(term->params[i]);break;
    }
}
static void process_byte(u8 c) {
    if(!online)return;
    if(term->escape==4) {
        if((term->osc && c==7) || (term->string_esc && c=='\\'))term->escape=0;
        term->string_esc=c==27;
        return;
    }
    if(c==27){term->escape=1;term->utf8=0;return;}
    if(term->escape==3){
        if(term->charset_bank<2)term->charsets[term->charset_bank]=(c=='0');
        term->escape=0;return;
    }
    if(term->escape==1) {
        term->escape=0;
        if(c=='['){memset(term->params,0,sizeof(term->params));term->nparam=term->prefix=0;term->escape=2;}
        else if(c=='(' || c==')' || c=='*' || c=='+' || c=='%' || c=='#'){term->charset_bank=c=='('?0:c==')'?1:2;term->escape=3;}
        else if(c==']' || c=='P' || c=='_' || c=='^'){term->osc=c==']';term->string_esc=0;term->escape=4;}
        else if(c=='7'){term->saved_x=term->col;term->saved_y=term->row;}
        else if(c=='8'){term->col=clamp(term->saved_x,columns-1);term->row=clamp(term->saved_y,rows-1);}
        else if(c=='D' || c=='M' || c=='E'){if(c=='E')term->col=0;feed(c=='M',0);}
        return;
    }
    if(term->escape==2){
        unsigned *v=&term->params[term->nparam];
        if(c>='0' && c<='9'){if(*v<1000)*v=*v*10+c-'0';}
        else if(c==';'){if(term->nparam<15)term->nparam++;}
        else if(c>=0x40 && c<=0x7e){term->escape=0;run_csi(c);}
        else term->prefix=c;
        return;
    }
    if (term->utf8) {
        if ((c&0xc0)==0x80){
            term->scalar=(term->scalar<<6)|(c&63);
            if(--term->utf8)return;
            unsigned u=term->scalar;
            c=(u==0x2500||u==0x2501||u==0x2550)?'-':(u==0x2502||u==0x2503||u==0x2551)?'|':
              (u>=0x2500&&u<=0x257f)?'+':(u>=0x2580&&u<=0x259f)?'#':'?';
        }else term->utf8=0;
    }
    if(c>=0xc2 && c<=0xf4){
        term->utf8=c<0xe0?1:c<0xf0?2:3;
        term->scalar=c&(term->utf8==1?31:term->utf8==2?15:7);return;
    }
    if(c=='\r'){term->col=0;return;}
    if(c=='\n'||c==11||c==12){term->col=0;feed(0,0);return;}
    if(c==8){if(term->col)term->col--;return;}
    if(c==9){term->col=clamp((term->col/8+1)*8,columns-1);return;}
    if(c==14||c==15){term->active_set=c==14;return;}
    if(c<32 || c>126)return;
    if(term->charsets[term->active_set]){
        if(c=='q')c='-';else if(c=='x')c='|';else if(c=='a')c='#';
        else if((c>='j'&&c<='n')||(c>='t'&&c<='w'))c='+';
    }
    if(term->col>=columns){term->col=0;feed(0,1);}
    term->cells[term->row][term->col]=(struct cell){c,term->reverse,term->color,term->paper};
    draw_cell(term->row,term->col);
    if (!term->nowrap || term->col+1<columns)term->col++;
}
void display_byte(u8 c) {
    if (!online)
        return;
    if (term->selected) {
        term->selected = term->selecting = 0;
        display_restore();
    }
    if (term->scroll) {
        term->scroll = 0;
        display_restore();
    }
    if (term->cursor && term->col < columns)
        draw_cell(term->row, term->col);
    process_byte(c);
    if (term->cursor && term->col < columns && !term->escape) {
        for (unsigned j = CH - 4; j < CH - 2; j++)
            for (unsigned i = 0; i < CW - 2; i++)
                frame[top + term->row * CH + j][origin + term->col * CW + i] = FG;
    }
}
u64 geometry(void) { return online ? (1UL << 32) | (rows << 16) | columns : (24UL << 16) | 80; }
u64 canvas_size(void) { return online ? (((height - BAR) / 2) << 16) | (pane_width / 2) : 0; }
long canvas_pointer(void) {
    if (!online || process_pane() != focus)
        return -1;
    long e = tablet_poll(width, height, BAR);
    if (e < 0 || !split)
        return e;
    unsigned x = (e & 4095) * 2;
    if (x < pane_origin || x >= pane_origin + pane_width)
        return e & ~(1UL << 25);
    return (e & ~4095UL) | ((x - pane_origin) / 2);
}
int present(const u64 *args, u64 arena, u64 bytes) {
    u64 p = args[0], x = args[1], y = args[2], w = args[3], h = args[4];
    if (!online || !w || !h || w > pane_width / 2 || h > (height - BAR) / 2 ||
        x > pane_width / 2 - w || y > (height - BAR) / 2 - h || p % 4 || p < arena ||
        p - arena > bytes || w * h * 4 > bytes - (p - arena))
        return -1;
    const u32 *src = (const u32 *)p;
    for (unsigned j = 0; j < h; j++) {
        u32 *dst = &frame[BAR + 2 * (y + j)][origin + 2 * x];
        for (unsigned i = 0; i < w; i++) {
            dst[2 * i] = dst[2 * i + 1] = *src++;
        }
        memcpy(dst + width, dst, w * 8);
    }
    return 0;
}
