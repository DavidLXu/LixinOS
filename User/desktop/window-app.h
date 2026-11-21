#pragma once
#include <lixin.h>
#ifdef __TINYC__
#include "/src/desktop/window-client.h"
#else
#include "window-client.h"
#endif

void *realloc(void *, unsigned long);
void free(void *);

typedef struct {
    int width, height;
    unsigned timer_ms;
    unsigned short *pixels;
    const char *copy;
    const unsigned char *paste;
    unsigned copy_size;
} AppWindow;

static inline unsigned short app_rgb(unsigned r, unsigned g, unsigned b) {
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static inline int app_hit(WindowEvent e, int x, int y, int w, int h) {
    return e.x >= x && e.x < x + w && e.y >= y && e.y < y + h;
}

static inline void app_rect(AppWindow *s, int x, int y, int w, int h, unsigned short color) {
    int right = x + w < s->width ? x + w : s->width;
    int bottom = y + h < s->height ? y + h : s->height;
    for (int j = y < 0 ? 0 : y; j < bottom; ++j)
        for (int i = x < 0 ? 0 : x; i < right; ++i)
            s->pixels[j * s->width + i] = color;
}

/* ASCII Menlo atlas used by LixinOS. A missing atlas leaves geometry intact. */
static inline void app_text(AppWindow *s, int x, int y, const char *label, unsigned short color) {
    static unsigned char atlas[95 * 18 * 22];
    static int ready;
    if (!ready) {
        int fd = lx_open("/src/gui/menlo.gray");
        if (fd < 0) { ready = -1; return; }
        for (unsigned offset = 0; offset < sizeof atlas;) {
            int n = lx_read(fd, atlas + offset, sizeof atlas - offset);
            if (n <= 0) { lx_close(fd); ready = -1; return; }
            offset += n;
        }
        lx_close(fd);
        ready = 1;
    }
    if (ready < 0) return;
    for (; *label && x < s->width; x += 11, ++label) {
        unsigned glyph = (unsigned char)*label;
        if (glyph < 32 || glyph > 126) glyph = '?';
        for (int j = 0; j < 22 && y + j < s->height; ++j)
            for (int i = 0; i < 11 && x + i < s->width; ++i)
                if (x + i >= 0 && y + j >= 0 && atlas[(glyph-32)*18*22+j*18+i] >= 128)
                    s->pixels[(y+j)*s->width + x+i] = color;
    }
}

static inline int app_send(const void *data, unsigned size) {
    const char *bytes = data;
    while (size) {
        int n = lx_write(1, bytes, size);
        if (n <= 0) return 0;
        bytes += n;
        size -= n;
    }
    return 1;
}

/* on_event returns nonzero when state changed. Configure always redraws. */
static inline int app_loop(int extended, int (*on_event)(AppWindow *, WindowEvent), void (*draw)(AppWindow *)) {
    AppWindow s = {0};
    int result = 0, escape = 0;
    unsigned short *runs=0;
    for (;;) {
        WindowEvent e;
        unsigned count = 0;
        while (count < sizeof e) {
            int n = lx_read(0, (char *)&e + count, sizeof e - count);
            if (n <= 0) { result = n < 0 || count != 0; goto done; }
            count += n;
        }
        int configure = e.type == WINDOW_CLIENT_CONFIGURE;
        if (configure) {
            if (e.x < 1 || e.y < 1 || e.x > 2048 || e.y > 1152) { result = 1; break; }
            void *pixels = realloc(s.pixels, (unsigned)e.x * e.y * sizeof *s.pixels);
            if (!pixels) { result = 1; break; }
            s.pixels = pixels;
            if (extended) {
                void *buffer=realloc(runs,(unsigned)e.x*e.y*sizeof *runs);
                if (buffer) runs=buffer;
                else { free(runs); runs=0; }
            }
            s.width = e.x;
            s.height = e.y;
        } else if (!s.pixels) { result = 1; break; }
        if (extended && e.type == WINDOW_CLIENT_CLOSE) break;
        unsigned char paste[WINDOW_CLIENT_CLIPBOARD_MAX];
        s.paste=paste;
        if (extended && e.type==WINDOW_CLIENT_PASTE) {
            if (e.value<=0 || e.value>WINDOW_CLIENT_CLIPBOARD_MAX) { result=1; break; }
            for (int got=0;got<e.value;) {
                int n=lx_read(0,paste+got,e.value-got);
                if (n<=0) { result=1; goto done; }
                got+=n;
            }
        }
        s.copy_size = 0;
        int consume = 0;
        if (extended && e.type == WINDOW_CLIENT_KEY) {
            if (!escape && e.value == 27) { escape=1; consume=1; }
            else if (escape==1 && (e.value=='[' || e.value=='O')) { escape=2; consume=1; }
            else if (escape) {
                if (escape==2) {
                    const char *codes="DCABHF";
                    int i=0;
                    while (codes[i] && codes[i]!=e.value) ++i;
                    if (codes[i]) e.value=300+i;
                }
                escape=0;
            }
        } else if (extended && escape && e.type==WINDOW_CLIENT_TIMER) {
            escape=0; e.type=WINDOW_CLIENT_KEY; e.value=27;
        }
        int changed = consume ? 0 : on_event(&s, e);
        if (extended && changed < 0) break;
        int repaint = changed || configure;
        WindowUpdate update = {0, 0, 0, 0, s.timer_ms > 65535 ? 65535 : s.timer_ms};
        if (repaint) {
            draw(&s);
            update.w = s.width;
            update.h = s.height;
        }
        if (escape) update.timer_ms=60;
        WindowReply reply = {update, s.copy_size, 0};
        unsigned pixels=(unsigned)update.w*update.h, words=0;
        if (runs && pixels) {
            for (unsigned i=0;i<pixels;) {
                if (words+2>=pixels) { words=0; break; }
                unsigned start=i++;
                while (i<pixels && i-start<65535 && s.pixels[i]==s.pixels[start]) ++i;
                runs[words++]=i-start;
                runs[words++]=s.pixels[start];
            }
            reply.encoded_size=2*words;
        }
        if (!app_send(&reply, extended ? sizeof reply : sizeof update) ||
            (repaint && !app_send(words ? runs : s.pixels, words ? reply.encoded_size : 2*pixels)) ||
            (extended && s.copy_size && !app_send(s.copy, s.copy_size))) {
            result = 1;
            break;
        }
    }
done:
    free(runs);
    free(s.pixels);
    return result;
}

static inline int app_run(int (*event)(AppWindow *, WindowEvent), void (*draw)(AppWindow *)) {
    return app_loop(0,event,draw);
}
static inline int app_run3(int (*event)(AppWindow *, WindowEvent), void (*draw)(AppWindow *)) {
    return app_loop(1,event,draw);
}
