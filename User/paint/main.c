#include "paint.h"
#include "../foreground/runtime.h"
#include "../console.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TOP = 66, CELL = 11, ROW = 22 };
static Drawing doc;
static Shape stroke;
static int tool, color, width = 4, down, bending, pointer_down, dirty, confirm_quit;
static int w, h, ox, oy, mx = -1, my;
static double scale;
static unsigned *base, *frame;
static char message[96];
static const char *filename;
static int changed = 1, previous_x = -1, previous_y = -1;
static const char *names[] = {"Pen",  "Line", "Arrow", "Curve", "Rect",
                              "Oval", "Save", "Undo",  "Quit"};
static const char *inks[] = {"Ink", "Red", "Gold", "Leaf", "Blue", "Plum", "Teal"};
static const int ansi[] = {37, 31, 33, 32, 34, 35, 36};
static void out(const char *s) { screen_write(s, strlen(s)); }
static void status(const char *s) { snprintf(message, sizeof message, "%s", s); }
static void bar(void) {
    char buf[100];
    out("\033[H\033[0m\033[K");
    for (int i = 0; i < 9; i++) {
        if (i == tool)
            out("\033[7m");
        out(names[i]);
        out("\033[0m ");
    }
    out("\033[2;1H\033[K");
    for (int i = 0; i < COLORS; i++) {
        snprintf(buf, sizeof buf, "\033[%d%sm%s\033[0m ", ansi[i], i == color ? ";7" : "", inks[i]);
        out(buf);
    }
    snprintf(buf, sizeof buf, "- %2d +", width);
    out(buf);
    out("\033[3;1H\033[K");
    snprintf(buf, sizeof buf, "%s%s", dirty ? "* " : "", message);
    buf[w / CELL - 1] = 0;
    out(buf);
    int room = w / CELL - (int)strlen(buf) - (int)strlen(filename) - 2;
    if (room > 0) {
        for (int i = 0; i < room; i++)
            out(" ");
        out(filename);
    }
}
static void background(void) {
    changed = 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            base[y * w + x] =
                (x >= ox && y >= oy && x < ox + ART_W * scale && y < oy + ART_H * scale) ? 0xffffff
                                                                                         : 0xdde2e7;
    for (size_t i = 0; i < doc.n; i++)
        raster(&doc.s[i], base, w, h, scale, ox, oy);
}
static void blit(unsigned *pixels, int x, int y, int width, int height) {
    unsigned long request[] = {(unsigned long)pixels, x, TOP + y, width, height};
    memcpy((void *)0x443d8000, request, sizeof request);
    console_call(20, 0);
}
// Pointer motion over a finished drawing touches only two small rectangles.
static void cursor_patch(int x, int y, int visible) {
    if (x < 0 || x >= w || y < 0 || y >= h)
        return;
    int left = x < 5 ? 0 : x - 5, top = y < 5 ? 0 : y - 5;
    int right = x + 6 > w ? w : x + 6, bottom = y + 6 > h ? h : y + 6;
    unsigned patch[11 * 11];
    for (int j = top; j < bottom; j++)
        for (int i = left; i < right; i++) {
            unsigned pixel = base[j * w + i];
            if (visible && ((i == x && abs(j - y) > 1) || (j == y && abs(i - x) > 1)))
                pixel ^= 0x909090;
            patch[(j - top) * (right - left) + i - left] = pixel;
        }
    // The blit interface accepts only arena-backed memory.
    memcpy(frame, patch, (right - left) * (bottom - top) * sizeof(*patch));
    blit(frame, left, top, right - left, bottom - top);
}
static void present(void) {
    if (!changed && !stroke.n) {
        cursor_patch(previous_x, previous_y, 0);
        cursor_patch(mx, my, 1);
    } else {
        memcpy(frame, base, (size_t)w * h * 4);
        if (stroke.n)
            raster(&stroke, frame, w, h, scale, ox, oy);
        if (mx >= 0 && my >= 0 && mx < w && my < h) {
            for (int d = -5; d <= 5; d++)
                if (abs(d) > 1) {
                    if (mx + d >= 0 && mx + d < w)
                        frame[my * w + mx + d] ^= 0x909090;
                    if (my + d >= 0 && my + d < h)
                        frame[(my + d) * w + mx] ^= 0x909090;
                }
        }
        blit(frame, 0, 0, w, h);
    }
    changed = 0;
    previous_x = mx;
    previous_y = my;
}
static void cancel(void) {
    changed = 1;
    free(stroke.p);
    stroke = (Shape){0};
    down = bending = 0;
}
static void commit(void) {
    if (!stroke.n)
        return;
    if (!append_shape(&doc, &stroke))
        status("Out of memory");
    else {
        raster(&doc.s[doc.n - 1], base, w, h, scale, ox, oy);
        dirty = 1;
        status("Drag to draw");
    }
    cancel();
    bar();
}
static void save(void) {
    size_t length;
    char *svg = make_svg(&doc, &length);
    int rc = svg ? save_file(svg, length) : 1;
    free(svg);
    if (!rc) {
        dirty = 0;
        status("Saved SVG");
    } else
        status(rc == 2   ? "File changed in other pane"
               : rc == 3 ? "Disk full"
               : rc == 1 ? "Drawing too large to save"
                         : "Save failed");
}
static int action(int i) {
    cancel();
    if (i < TOOLS) {
        tool = i;
        status(i == CURVE ? "Drag endpoints, then click to bend" : "Drag to draw");
    } else if (i == 6)
        save();
    else if (i == 7) {
        if (doc.n) {
            free(doc.s[--doc.n].p);
            dirty = 1;
            background();
        }
        status("Undo");
    } else if (i == 8) {
        if (!dirty || confirm_quit)
            return 1;
        confirm_quit = 1;
        status("Unsaved: Q again to discard, S to save");
        bar();
        return 0;
    }
    confirm_quit = 0;
    bar();
    return 0;
}
static int click_bar(int x, int y) {
    int col = x / CELL, start = 0;
    if (y < ROW) {
        for (int i = 0; i < 9; i++) {
            int end = start + strlen(names[i]);
            if (col >= start && col < end)
                return action(i);
            start = end + 1;
        }
    } else if (y < 2 * ROW) {
        confirm_quit = 0;
        for (int i = 0; i < COLORS; i++) {
            int end = start + strlen(inks[i]);
            if (col >= start && col < end) {
                color = i;
                bar();
                return 0;
            }
            start = end + 1;
        }
        if (col == start && width > 1)
            --width;
        if (col == start + 5 && width < 32)
            ++width;
        bar();
    }
    return 0;
}
static Point point(int x, int y) {
    return (Point){fmax(0, fmin(ART_W - 1, (x - ox) / scale)),
                   fmax(0, fmin(ART_H - 1, (y - oy) / scale))};
}
static void update(Point p) {
    if (!stroke.n)
        return;
    if (tool == PEN) {
        Point last = stroke.p[stroke.n - 1];
        if ((last.x != p.x || last.y != p.y) && !add_point(&stroke, p)) {
            cancel();
            status("Out of memory");
            bar();
        }
    } else if (tool == CURVE) {
        if (bending)
            stroke.p[1] = p;
        else {
            stroke.p[2] = p;
            stroke.p[1] = (Point){(stroke.p[0].x + p.x) / 2, (stroke.p[0].y + p.y) / 2};
        }
    } else
        stroke.p[1] = p;
}
static int mouse(unsigned long e) {
    if (e & (1UL << 28)) {
        cancel();
        pointer_down = 0;
        mx = -1;
        return 0;
    }
    int x = e & 4095, y = (e >> 12) & 4095, held = (e >> 24) & 1, inside = (e >> 25) & 1;
    int pressed = held && !pointer_down, released = !held && pointer_down;
    pointer_down = held;
    if (inside && (e & (3UL << 26))) {
        confirm_quit = 0;
        width = fmax(1, fmin(32, width + ((e & (1UL << 26)) ? 1 : -1)));
        bar();
    }
    mx = inside ? x : -1;
    my = y - TOP;
    if (pressed && inside && y < TOP)
        return click_bar(x, y);
    Point p = point(x, y - TOP);
    if (pressed && inside && y >= TOP && x >= ox && y - TOP >= oy && x < ox + ART_W * scale &&
        y - TOP < oy + ART_H * scale) {
        confirm_quit = 0;
        if (bending) {
            update(p);
            commit();
            return 0;
        }
        stroke = (Shape){.tool = tool, .width = width, .color = color};
        int count = tool == PEN ? 1 : tool == CURVE ? 3 : 2;
        for (int i = 0; i < count; i++)
            if (!add_point(&stroke, p)) {
                cancel();
                return 0;
            }
        down = 1;
    }
    if (down || (bending && inside && y >= TOP))
        update(p);
    if (released && down) {
        down = 0;
        if (tool == CURVE) {
            bending = 1;
            status("Click to set the curve control point");
            bar();
        } else
            commit();
    }
    return 0;
}
int paint(const char *name, const char *text, size_t length) {
    filename = strrchr(name, '/');
    filename = filename ? filename + 1 : name;
    unsigned long geometry = console_call(19, 0);
    if (!geometry || geometry == (unsigned long)-1) {
        out("paint: tablet unavailable\r\n");
        return 1;
    }
    if (!load_svg(&doc, text, length)) {
        out("paint: unsupported SVG; use a new filename or a drawing saved by paint\r\n");
        return 1;
    }
    w = geometry & 65535;
    h = (geometry >> 16) - TOP;
    base = malloc((size_t)w * h * 4);
    frame = malloc((size_t)w * h * 4);
    if (!base || !frame) {
        out("paint: out of memory\r\n");
        return 1;
    }
    scale = fmin((double)w / ART_W, (double)h / ART_H);
    ox = (w - ART_W * scale) / 2;
    oy = (h - ART_H * scale) / 2;
    out("\033[2J\033[H");
    status("Drag to draw   Wheel: width");
    bar();
    background();
    present();
    for (;;) {
        unsigned long e;
        int redraw = 0;
        while ((e = console_call(21, 0)) != (unsigned long)-1) {
            if (mouse(e))
                goto done;
            redraw = 1;
        }
        if (redraw)
            present();
        int k = read_timed(8);
        if (k == 256)
            continue;
        if (k != 'q' && k != 'Q' && k != 17)
            confirm_quit = 0;
        if (k == 'q' || k == 'Q' || k == 17) {
            if (action(8))
                break;
        } else if (k == 's' || k == 19)
            action(6);
        else if (k == 'z' || k == 26)
            action(7);
        else if (k >= '1' && k <= '6')
            action(k - '1');
        else if (k == 'c') {
            color = (color + 1) % COLORS;
            bar();
        } else if (k == '+' || k == '-') {
            width = fmax(1, fmin(32, width + (k == '+' ? 1 : -1)));
            bar();
        } else if (k == 27) {
            cancel();
            confirm_quit = 0;
            status("Drag to draw");
            bar();
        }
        present();
    }
done:
    out("\033[0m\033[2J\033[H");
    return 0;
}
