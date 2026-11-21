#include <lixin-gui.h>
#include <stdio.h>

#include "calculator-model.h"
static struct lx_font font;
static struct lx_canvas canvas;
static struct lx_gui_cursor cursor;
static struct lx_button buttons[COUNT];
static void draw(unsigned x, unsigned y) {
    lx_gui_rect(&canvas, 0, 0, canvas.width, canvas.height, 0x182730);
    lx_gui_rect(&canvas, x, y, 376, 378, 0xe8ede8);
    lx_gui_text(&canvas, &font, x+18, y+12, 23, "Calculator", 0x233b43);
    lx_gui_rect(&canvas, x+16, y+47, 344, 52, 0x223b43);
    lx_gui_text(&canvas, &font, x+27, y+61, 35,
                error == 1 ? "Cannot divide by zero" : error ? "Result too large" : input, 0xeaf6ee);
    for (unsigned i = 0; i < COUNT; ++i)
        lx_gui_button(&canvas, &font, &buttons[i], i == 18 ? 0x24736f : 0x35535d, 0xffffff);
    lx_gui_present(&canvas);
    lx_gui_cursor_show(&canvas, &cursor);
}

int main(void) {
    if (lx_gui_font(&font) || lx_gui_open(&canvas)) return 1;
    unsigned x = (canvas.width - 376) / 2, y = (canvas.height - 378) / 2;
    for (unsigned i = 0; i < COUNT; ++i)
        buttons[i] = (struct lx_button){x+16+i%4*87, y+112+i/4*49, 82, 43, labels[i]};
    draw(x, y);
    int held = 0;
    for (;;) {
        int key = lx_timed_byte(12);
        if (key == 27 || key == 'q') return 0;
        if (key == 13 || key == 10) key = '=';
        struct lx_pointer pointer;
        while (lx_gui_pointer(&pointer)) {
            if (pointer.cancelled) held = 0;
            else if (pointer.down && !held)
                for (unsigned i = 0; i < COUNT; ++i)
                    if (lx_gui_hit(&buttons[i], &pointer)) key = keys[i];
            held = pointer.down;
            lx_gui_cursor_move(&canvas, &cursor, &pointer);
        }
        if (key == 'q') return 0;
        if (key >= 0 && key < 256) { press(key); draw(x, y); }
    }
}
