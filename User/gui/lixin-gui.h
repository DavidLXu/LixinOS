#pragma once
int lx_open(const char *path);
int lx_read(int fd, void *buffer, int count);
int lx_close(int fd);
int lx_timed_byte(unsigned ms);

/* Foreground QEMU canvas. Keep stdout attached to the terminal and return
   from main to restore it. Colors are logical XRGB pixels (0x00RRGGBB). */
struct lx_canvas {
    unsigned width, height;
    unsigned *pixels;
};

struct lx_pointer {
    unsigned x, y;
    int inside, down, wheel, cancelled, copy;
};

unsigned long lx_canvas_size(void);
int lx_canvas_present(const unsigned *pixels, unsigned x, unsigned y,
                      unsigned width, unsigned height);
long lx_canvas_pointer(void);

static inline int lx_gui_open(struct lx_canvas *canvas) {
    unsigned long size = lx_canvas_size();
    if (!size || size == (unsigned long)-1)
        return -1;
    canvas->width = size & 65535;
    canvas->height = size >> 16;
    canvas->pixels = (unsigned *)0x68000000UL;
    return 0;
}

static inline void lx_gui_rect(struct lx_canvas *canvas, unsigned x, unsigned y,
                               unsigned width, unsigned height, unsigned color) {
    if (x >= canvas->width || y >= canvas->height)
        return;
    if (width > canvas->width - x) width = canvas->width - x;
    if (height > canvas->height - y) height = canvas->height - y;
    for (unsigned row = y; row < y + height; ++row)
        for (unsigned col = x; col < x + width; ++col)
            canvas->pixels[row * canvas->width + col] = color;
}

static inline int lx_gui_present(const struct lx_canvas *canvas) {
    return lx_canvas_present(canvas->pixels, 0, 0, canvas->width, canvas->height);
}

static inline int lx_gui_decode_pointer(struct lx_pointer *pointer, long event) {
    if (event < 0)
        return 0;
    pointer->x = event & 4095;
    pointer->y = (event >> 12) & 4095;
    pointer->down = (event >> 24) & 1;
    pointer->inside = (event >> 25) & 1;
    pointer->wheel = ((event >> 26) & 1) - ((event >> 27) & 1);
    pointer->cancelled = (event >> 28) & 1;
    pointer->copy = (event >> 30) & 1;
    return 1;
}

static inline int lx_gui_pointer(struct lx_pointer *pointer) {
    return lx_gui_decode_pointer(pointer, lx_canvas_pointer());
}

/* Menlo atlas shared with the system desktop. Load once per app. */
#define LX_GUI_CHAR_W 9
#define LX_GUI_CHAR_H 22
struct lx_font { unsigned char alpha[95 * 18 * LX_GUI_CHAR_H]; };

static inline int lx_gui_font(struct lx_font *font) {
    int fd = lx_open("/src/gui/menlo.gray");
    if (fd < 0) return -1;
    unsigned n = 0;
    while (n < sizeof(font->alpha)) {
        int got = lx_read(fd, font->alpha + n, sizeof(font->alpha) - n);
        if (got <= 0) break;
        n += got;
    }
    lx_close(fd);
    return n == sizeof(font->alpha) ? 0 : -1;
}

static inline void lx_gui_text(struct lx_canvas *canvas, const struct lx_font *font,
                               unsigned x, unsigned y, unsigned cells, const char *text,
                               unsigned color) {
    for (; cells && *text && x + LX_GUI_CHAR_W <= canvas->width; --cells, x += LX_GUI_CHAR_W) {
        unsigned char ch = *text++;
        if (ch < 32 || ch >= 127) ch = '?';
        const unsigned char *glyph = font->alpha + (ch - 32) * 18 * LX_GUI_CHAR_H;
        for (unsigned row = 0; row < LX_GUI_CHAR_H && y + row < canvas->height; ++row) {
            for (unsigned col = 0; col < LX_GUI_CHAR_W; ++col) {
                unsigned *pixel = canvas->pixels + (y + row) * canvas->width + x + col;
                unsigned a = glyph[row * 18 + col];
                unsigned rb = (((*pixel & 0xff00ff) * (256 - a) + (color & 0xff00ff) * a) >> 8) & 0xff00ff;
                unsigned g = (((*pixel & 0xff00) * (256 - a) + (color & 0xff00) * a) >> 8) & 0xff00;
                *pixel = rb | g;
            }
        }
    }
}

struct lx_button { unsigned x, y, w, h; const char *label; };

static inline int lx_gui_hit(const struct lx_button *button, const struct lx_pointer *pointer) {
    return pointer->inside && pointer->x >= button->x && pointer->x - button->x < button->w
           && pointer->y >= button->y && pointer->y - button->y < button->h;
}

static inline void lx_gui_button(struct lx_canvas *canvas, const struct lx_font *font,
                                 const struct lx_button *button, unsigned fill, unsigned ink) {
    unsigned n = 0;
    while (n < button->w / LX_GUI_CHAR_W && button->label[n]) ++n;
    lx_gui_rect(canvas, button->x, button->y, button->w, button->h, fill);
    lx_gui_text(canvas, font, button->x + (button->w - n * LX_GUI_CHAR_W) / 2,
                button->y + (button->h - LX_GUI_CHAR_H) / 2, n, button->label, ink);
}

/* The tile follows the packed canvas in the arena. Moving it does not repaint
   the app or bake the pointer into its backing pixels. */
struct lx_gui_cursor { unsigned x, y; int visible, text; };

static inline void lx_gui_cursor_tile(struct lx_canvas *canvas, unsigned x, unsigned y, int draw, int text) {
    if (x >= canvas->width || y >= canvas->height) return;
    unsigned w = canvas->width - x < 12 ? canvas->width - x : 12;
    unsigned h = canvas->height - y < 16 ? canvas->height - y : 16;
    unsigned *tile = canvas->pixels + canvas->width * canvas->height;
    for (unsigned row = 0; row < h; ++row) {
        for (unsigned col = 0; col < w; ++col) {
            unsigned pixel = canvas->pixels[(row + y) * canvas->width + x + col];
            if (draw && text && row < 15 && col >= 1 && col <= 7 &&
                (row < 3 || row >= 12 || (col >= 3 && col <= 5))) {
                int light = (row == 1 || row == 13) ? col > 1 && col < 7 :
                            col == 4 && row != 0 && row != 14;
                pixel = light ? 0xf5fafb : 0x14212a;
            } else if (draw && !text && col <= row / 2) {
                pixel = col == 0 || col == row / 2 || row == 15 ? 0x14212a : 0xf5fafb;
            }
            tile[row * w + col] = pixel;
        }
    }
    lx_canvas_present(tile, x, y, w, h);
}

static inline void lx_gui_cursor_show(struct lx_canvas *canvas, const struct lx_gui_cursor *cursor) {
    if (cursor->visible) lx_gui_cursor_tile(canvas, cursor->x, cursor->y, 1, cursor->text);
}

static inline void lx_gui_cursor_move_as(struct lx_canvas *canvas, struct lx_gui_cursor *cursor,
                                         const struct lx_pointer *pointer, int text) {
    if (cursor->visible) lx_gui_cursor_tile(canvas, cursor->x, cursor->y, 0, cursor->text);
    cursor->visible = pointer->inside && !pointer->cancelled;
    cursor->text = text;
    cursor->x = text && pointer->x >= 4 ? pointer->x - 4 : pointer->x;
    cursor->y = text && pointer->y >= 7 ? pointer->y - 7 : pointer->y;
    lx_gui_cursor_show(canvas, cursor);
}

static inline void lx_gui_cursor_move(struct lx_canvas *canvas, struct lx_gui_cursor *cursor,
                                      const struct lx_pointer *pointer) {
    lx_gui_cursor_move_as(canvas, cursor, pointer, 0);
}
