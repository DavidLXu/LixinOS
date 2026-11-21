#pragma once
#include "../gui/lixin-gui.h"

struct browser_window {
    struct lx_canvas canvas;
    int hosted, x, y, w, h, mouse_x, mouse_y, inside, down, dragging, dx, dy;
};
enum { WEB_MARGIN=14, WEB_TOP=84, WEB_LINE=22, WEB_CHAR=9 };
int window_open(struct browser_window *);
void window_frame(struct browser_window *, const char *title, const char *address, const char *status);
void window_char(struct browser_window *, int col, int row, unsigned ch, int is_link);
void window_present(struct browser_window *);
