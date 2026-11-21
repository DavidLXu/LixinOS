/* tcc /src/desktop/stopwatch.c -o /bin/my-watch.win2 */
#include "window-client.h"
#ifdef __TINYC__
#include <lixin.h>
long lx_console_call(long, long);
#define clock_ms() lx_console_call(38, 0)
#else
#include "../console.h"
#include "../tcc/lixin.h"
#define clock_ms() console_call(38, 0)
#endif

enum { BACK = 0x1127, TRACK = 0x2249, DIGIT = 0xe77a, PLAY = 0x568f, PAUSE = 0xf4e8 };
typedef struct {
    int w, h, scale, left, top, running, down;
    long elapsed, started;
    int digits[7];
} Watch;

static int write_all(const void *data, int bytes) {
    const char *p = data;
    while (bytes) {
        int n = lx_write(1, p, bytes);
        if (n <= 0) return 0;
        p += n;
        bytes -= n;
    }
    return 1;
}

static unsigned short color(const Watch *s, int x, int y) {
    /* Each glyph consists of five three-bit rows, from top to bottom. */
    static const unsigned short glyphs[] = {
        075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717,
        002020, 000002
    };
    if (x >= s->w-46 && x < s->w-14 && y >= 12 && y < 40) {
        int dx = x-(s->w-46), dy = y-12;
        int symbol = s->running ? ((dx >= 10 && dx < 14) || (dx >= 18 && dx < 22)) && dy >= 7 && dy < 21
                                : dx >= 11 && dx <= 21 && (dy >= 5 + (dx-11)/2 && dy <= 22 - (dx-11)/2);
        return symbol ? BACK : s->running ? PAUSE : PLAY;
    }
    if (x >= s->left && y >= s->top && y < s->top+5*s->scale) {
        int col = (x-s->left)/s->scale, cell = col/4;
        if (cell < 7 && col%4 < 3) {
            int row = (y-s->top)/s->scale;
            unsigned mask = 1u << ((4-row)*3 + 2-col%4);
            return glyphs[s->digits[cell]] & mask ? DIGIT : TRACK;
        }
    }
    return BACK;
}

static int reply(const Watch *s, WindowUpdate u) {
    unsigned short block[2048];
    int n = 0;
    if (!write_all(&u, sizeof u)) return 0;
    for (int y = u.y; y < u.y+u.h; ++y) {
        for (int x = u.x; x < u.x+u.w; ++x) {
            block[n++] = color(s, x, y);
            if (n == 2048) {
                if (!write_all(block, sizeof block)) return 0;
                n = 0;
            }
        }
    }
    return write_all(block, n * sizeof block[0]);
}

int main(void) {
    Watch s = {0};
    for (;;) {
        WindowEvent e;
        for (unsigned i = 0; i < sizeof e;) {
            int n = lx_read(0, (char *)&e+i, sizeof e-i);
            if (n <= 0) return i != 0 || n < 0;
            i += n;
        }
        long now = clock_ms();
        int full = e.type == WINDOW_CLIENT_CONFIGURE;
        if (full) {
            s.w = e.x;
            s.h = e.y;
            s.scale = (s.w-24)/27;
            if (s.scale > (s.h-58)/5) s.scale = (s.h-58)/5;
            if (s.scale < 1) s.scale = 1;
            s.left = (s.w-27*s.scale)/2;
            s.top = 40 + (s.h-40-5*s.scale)/2;
        }
        int toggle = e.type == WINDOW_CLIENT_KEY && e.value == ' ';
        if (e.type == WINDOW_CLIENT_POINTER) {
            toggle = e.value && !s.down && e.x >= s.w-46 && e.x < s.w-14 && e.y >= 12 && e.y < 40;
            s.down = e.value;
        }
        if (toggle) {
            if (s.running) s.elapsed += now - s.started;
            else s.started = now;
            s.running = !s.running;
        }
        if (e.type == WINDOW_CLIENT_KEY && (e.value == 'r' || e.value == 'R')) {
            s.elapsed = 0;
            s.started = now;
        }
        long ticks = (s.elapsed + (s.running ? now-s.started : 0))/100;
        int digits[] = {ticks/6000%10, ticks/600%10, 10, ticks/100%6, ticks/10%10, 11, ticks%10};
        int first = 7, last = -1;
        for (int i = 0; i < 7; ++i) {
            if (digits[i] != s.digits[i]) {
                if (first == 7) first = i;
                last = i;
                s.digits[i] = digits[i];
            }
        }
        WindowUpdate u = {0};
        if (full || (toggle && last >= first)) {
            u.w = s.w; u.h = s.h;
        } else if (toggle) {
            u.x = s.w-46; u.y = 12; u.w = 32; u.h = 28;
        } else if (first <= last) {
            u.x = s.left + first*4*s.scale; u.y = s.top;
            u.w = ((last-first)*4+3)*s.scale; u.h = 5*s.scale;
        }
        u.timer_ms = s.running ? 100 : 0;
        if (!reply(&s, u)) return 0;
    }
}

#ifndef __TINYC__
__attribute__((section(".text.start"), noreturn)) void _start(void) { finish(main()); }
#endif
