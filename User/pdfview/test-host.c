/* Drive the graphical viewer through Desktop's normal Win3 protocol. */
#include <lixin.h>
#include "/src/console.h"
typedef unsigned long size_t;
#define NULL ((void*)0)
#include <string.h>
#include <stdio.h>
#include "/src/gui/text-clipboard.h"
#include "/src/desktop/window-client-host.h"

static WindowClient viewer;
static int event(int kind, int x, int y, int value) {
    client_event(&viewer, (WindowEvent){kind, x, y, value});
    long deadline = console_call(38, 0) + 60000;
    while (viewer.count || viewer.busy) {
        if (client_tick(&viewer, 0) < 0 || console_call(38, 0) >= deadline) return 0;
        if (viewer.busy && lx_poll(viewer.output, LX_READABLE, 0) <= 0) console_call(43, 1);
    }
    return 1;
}

static int count_color(int red, int left, int right, int step) {
    int count = 0;
    for (int y = 80; y < viewer.height; y += step) {
        for (int x = left; x < right; x += step) {
            unsigned pixel = viewer.pixels[y * viewer.width + x];
            int r = pixel >> 11, b = pixel & 31;
            if (red ? r > b + 9 : b > r + 9) ++count;
        }
    }
    return count;
}

/* The thumbnail sidebar is on the left, so a red page-2 thumbnail can only
 * appear there while page 1 is in the main canvas. */
static int find_thumbnail(int red) {
    for (int frame = 0; frame < 60; ++frame) {
        console_call(43, 40);
        if (!event(WINDOW_CLIENT_TIMER, 0, 0, 0)) break;
        if (count_color(red, 0, 150, 2) > 40) return 1;
    }
    printf("Missing page %d thumbnail (%d)\n", red ? 2 : 1, count_color(red, 0, 150, 2));
    return 0;
}

static int thumbnail_gone(int red) {
    for (int frame = 0; frame < 30; ++frame) {
        if (!event(WINDOW_CLIENT_TIMER, 0, 0, 0)) break;
        if (count_color(red, 0, 150, 2) < 10) return 1;
    }
    printf("Thumbnail stayed visible (%d)\n", count_color(red, 0, 150, 2));
    return 0;
}

static int find_color(int red) {
    for (int frame = 0; frame < 50; ++frame) {
        if (!event(WINDOW_CLIENT_TIMER, 0, 0, 0)) break;
        unsigned count = 0;
        for (int y = 80; y < viewer.height; y += 8) {
            for (int x = 160; x < viewer.width; x += 8) {
                unsigned pixel = viewer.pixels[y * viewer.width + x];
                int r = pixel >> 11, b = pixel & 31;
                if (red ? r > b + 9 : b > r + 9) ++count;
            }
        }
        if (count > 100) return 1;
    }
    printf("Missing page color %d at %dx%d (%s)\n", red, viewer.width, viewer.height, viewer.title);
    return 0;
}

static int click(int x, int y) {
    return event(WINDOW_CLIENT_POINTER, x, y, 1) && event(WINDOW_CLIENT_POINTER, x, y, 0);
}

/* Blue banner margins let us observe fit, zoom and scroll from the public framebuffer. */
static int banner_top(void) {
    for (int y = 80; y < viewer.height; y += 3) {
        for (int x = 160; x < viewer.width; x += 3) {
            unsigned p = viewer.pixels[y * viewer.width + x];
            if ((int)(p & 31) - (int)(p >> 11) > 9) {
                return y;
            }
        }
    }
    return viewer.height;
}

static int edge_moved(int old) {
    for (int frame = 0; frame < 40; ++frame) {
        if (!event(WINDOW_CLIENT_TIMER, 0, 0, 0)) return 0;
        int delta = banner_top() - old;
        if (delta > 8 || delta < -8) return 1;
        console_call(43, 8);
    }
    printf("Banner still at %d after input (was %d)\n", banner_top(), old);
    return 0;
}

static int close_window(void) {
    client_event(&viewer, (WindowEvent){WINDOW_CLIENT_CLOSE, 0, 0, 0});
    long deadline = console_call(38, 0) + 15000;
    while (viewer.pid && console_call(38, 0) < deadline) {
        client_tick(&viewer, 0);
        if (!viewer.pid) break;
        console_call(43, 1);
    }
    if (viewer.pid) { client_shutdown(&viewer); return 0; }
    return 1;
}

int main(void) {
    if (!client_open_streams(&viewer, "/", "/bin/pdfview.win3", "/reading.pdf", 2)) return 1;
    if (!event(WINDOW_CLIENT_CONFIGURE, 800, 520, 15) || !find_color(0)) return 2;
    if (!find_thumbnail(1)) return 9;
    if (!event(WINDOW_CLIENT_KEY, 2, 0, 2) || !thumbnail_gone(1)) return 10;
    if (!event(WINDOW_CLIENT_KEY, 2, 0, 2) || !find_thumbnail(1)) return 10;
    int top = banner_top();
    if (!click(354, 56) || !edge_moved(top)) return 6;
    top = banner_top();
    if (!click(420, 56) || !edge_moved(top)) return 7;
    top = banner_top();
    if (!event(WINDOW_CLIENT_POINTER, 300, 300, 0) ||
        !event(WINDOW_CLIENT_WHEEL, 300, 300, -1) || !edge_moved(top)) return 8;
    if (!click(230, 56)) return 3;
    if (!find_color(1)) return 4;
    if (!event(WINDOW_CLIENT_CONFIGURE, 640, 400, 15) || !find_color(1)) return 5;
    /* The page number must survive a full restart. */
    console_call(43, 700);
    if (!close_window()) return 11;
    if (!client_open_streams(&viewer, "/", "/bin/pdfview.win3", "/reading.pdf", 2)) return 11;
    if (!event(WINDOW_CLIENT_CONFIGURE, 800, 520, 15) || !find_color(1)) return 12;
    if (!close_window()) return 13;
    printf("PDF window passed\n");
    return 0;
}
