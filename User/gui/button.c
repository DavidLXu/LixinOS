#include <lixin-gui.h>
#include <stdio.h>

enum { BUTTON_WIDTH = 144, BUTTON_HEIGHT = 64 };

static void draw(struct lx_canvas *canvas, unsigned x, unsigned y, unsigned clicks) {
    static const unsigned char label[][5] = {
        {7, 4, 4, 4, 7}, {4, 4, 4, 4, 7}, {7, 2, 2, 2, 7},
        {7, 4, 4, 4, 7}, {5, 5, 6, 5, 5},
    };
    lx_gui_rect(canvas, 0, 0, canvas->width, canvas->height, 0x17232e);
    lx_gui_rect(canvas, x, y + 4, BUTTON_WIDTH, BUTTON_HEIGHT, 0x0e1821);
    lx_gui_rect(canvas, x, y, BUTTON_WIDTH, BUTTON_HEIGHT,
                clicks & 1 ? 0xa05228 : 0x246c71);
    for (unsigned i = 0; i < 5; ++i)
        for (unsigned row = 0; row < 5; ++row)
            for (unsigned col = 0; col < 3; ++col)
                if (label[i][row] & (4 >> col))
                    lx_gui_rect(canvas, x + 44 + i * 12 + col * 3, y + 24 + row * 3,
                                3, 3, 0xf9f2df);
    for (unsigned i = 0; i < 8; ++i)
        lx_gui_rect(canvas, x + 14 + i * 15, y + 84, 10, 10,
                    i < clicks % 9 ? 0xeab775 : 0x364958);
    lx_gui_present(canvas);
}

int main(void) {
    struct lx_canvas canvas;
    if (lx_gui_open(&canvas) < 0) {
        puts("button-demo requires a foreground QEMU canvas.");
        return 1;
    }
    unsigned x = (canvas.width - BUTTON_WIDTH) / 2;
    unsigned y = (canvas.height - BUTTON_HEIGHT - 30) / 2;
    unsigned clicks = 0;
    int held = 0, armed = 0;
    draw(&canvas, x, y, clicks);
    for (;;) {
        int key = lx_timed_byte(12);
        if (key == 27 || key == 'q')
            return 0;
        struct lx_pointer pointer;
        while (lx_gui_pointer(&pointer)) {
            if (pointer.cancelled) {
                held = armed = 0;
                continue;
            }
            int hit = pointer.inside && pointer.x >= x && pointer.x < x + BUTTON_WIDTH
                       && pointer.y >= y && pointer.y < y + BUTTON_HEIGHT;
            if (pointer.down && !held)
                armed = hit;
            if (!pointer.down && held && armed && hit)
                draw(&canvas, x, y, ++clicks);
            held = pointer.down;
        }
    }
}
